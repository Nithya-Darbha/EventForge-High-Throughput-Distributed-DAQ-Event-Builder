#include "builder/receiver.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include "daq/clock.hpp"
#include "daq/crc32c.hpp"
#include "daq/net.hpp"

namespace daq {

Receiver::Receiver(const BuilderConfig& c, Shared& s, std::vector<std::unique_ptr<Shard>>& sh,
                   TtcHost& t, StartFn start, DoneFn trigDone)
    : cfg(c), shared(s), shards(sh), ttc(t), onStart(std::move(start)), triggerDone(std::move(trigDone)),
      sourceFd(c.sources,-1), track(c.sources) {
    grantStep=cfg.credits/8>0 ? cfg.credits/8 : 1;  //dont send a CREDIT msg for every single frag
    listenFd=net::listenTcp(cfg.port);
    ep=epoll_create1(EPOLL_CLOEXEC);
    epoll_event ev{};
    ev.events=EPOLLIN;
    ev.data.fd=listenFd;
    epoll_ctl(ep,EPOLL_CTL_ADD,listenFd,&ev);
    //sinks/shards poke this when credits are ready to go back
    epoll_event wev{};
    wev.events=EPOLLIN;
    wev.data.fd=shared.wakeFd;
    epoll_ctl(ep,EPOLL_CTL_ADD,shared.wakeFd,&wev);
}

Receiver::~Receiver() {
    for(auto& [fd,c]:conns){
        close(fd);
    }
    if(ep>=0) close(ep);
    if(listenFd>=0) close(listenFd);
}

/*
loop
  epoll (1ms) -> accept / read
  retry conns that stalled on an empty pool
  send credits back
  end: trigger stopped + every source gone
*/
void Receiver::run(const std::atomic<bool>& stop) {
    nameThread("eb-recv");
    std::vector<epoll_event> evs(64);
    uint64_t began=nowNs();

    while(!stop.load(std::memory_order_relaxed)){
        int n=epoll_wait(ep,evs.data(),static_cast<int>(evs.size()),1);
        for(int i=0;i<n;i++){
            int fd=evs[static_cast<size_t>(i)].data.fd;
            if(fd==listenFd){
                accept();
                continue;
            }
            if(fd==shared.wakeFd){
                uint64_t cnt=0;
                ssize_t r=read(shared.wakeFd,&cnt,sizeof(cnt));  //just clear it, grantCredits below does the work
                (void)r;
                continue;
            }
            auto it=conns.find(fd);
            if(it!=conns.end()) onReadable(it->second);
        }

        //pool had no free slot last time, try again (slots come back as sinks finish)
        std::vector<int> toClose;
        for(auto& [fd,c]:conns){
            if(!c.stalled) continue;
            Parse p=parse(c);
            if(p==Parse::ERROR){
                toClose.push_back(fd);
            }
            else if(p==Parse::OK){
                c.stalled=false;
                setReading(c,true);
            }
        }
        for(int fd:toClose) closeConn(fd);

        grantCredits();

        bool allGone=connected.get()==0;
        bool runOver=runStarted() && triggerDone() && allGone;
        if(runOver) break;
        if(nsToS(nowNs()-began)>cfg.maxRunS){
            std::fprintf(stderr,"builder: max run time hit, stopping\n");
            break;
        }
    }
} /* Receiver::run() */

void Receiver::accept() {
    while(true){
        int fd=accept4(listenFd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
        if(fd<0) break;
        net::setRcvBuf(fd,4<<20);
        net::setNoDelay(fd);
        epoll_event ev{};
        ev.events=EPOLLIN|EPOLLRDHUP;
        ev.data.fd=fd;
        epoll_ctl(ep,EPOLL_CTL_ADD,fd,&ev);
        conns[fd].fd=fd;
    }
}

void Receiver::setReading(Conn& c, bool on) {
    if(c.reading==on) return;
    epoll_event ev{};
    ev.events=on ? (EPOLLIN|EPOLLRDHUP) : 0;
    ev.data.fd=c.fd;
    epoll_ctl(ep,EPOLL_CTL_MOD,c.fd,&ev);
    c.reading=on;
}

/*
max 4 recv per wakeup so one busy source cant starve the others
recv 0 -> peer closed
parse after every recv
*/
void Receiver::onReadable(Conn& c) {
    for(int i=0;i<4;i++){
        if(c.rx.size()-c.have<64*1024) c.rx.resize(c.have+256*1024);
        ssize_t n=recv(c.fd,c.rx.data()+c.have,c.rx.size()-c.have,0);
        if(n==0){
            closeConn(c.fd);
            return;
        }
        if(n<0){
            if(errno==EAGAIN || errno==EINTR) return;
            closeConn(c.fd);
            return;
        }
        c.have+=static_cast<size_t>(n);
        Parse p=parse(c);
        if(p==Parse::ERROR){
            protocolErrors.add();
            std::fprintf(stderr,"builder: protocol error from source %d, closing\n",c.source);
            closeConn(c.fd);
            return;
        }
        if(p==Parse::STALL){
            //stop reading this conn till the pool has room, tcp pushes back on the frontend
            c.stalled=true;
            setReading(c,false);
            return;
        }
    }
}

/*
while a whole header is there
  bad header -> ERROR (no resync attempts, desynced stream = drop the conn)
  whole body there? no -> wait for more bytes
  handle it, STALL -> stop here, the msg stays in rx for later
move leftover bytes to the front
*/
Receiver::Parse Receiver::parse(Conn& c) {
    size_t off=0;
    Parse result=Parse::OK;
    while(c.have-off>=sizeof(MsgHeader)){
        auto h=get<MsgHeader>(c.rx.data()+off);
        if(validate(h)!=HeaderCheck::OK) return Parse::ERROR;
        size_t need=sizeof(MsgHeader)+h.bodyLen;
        if(c.have-off<need) break;
        Parse p=handleMsg(c,h,c.rx.data()+off+sizeof(MsgHeader));
        if(p==Parse::ERROR) return Parse::ERROR;
        if(p==Parse::STALL){
            result=Parse::STALL;
            break;
        }
        off+=need;
    }
    if(off){
        std::memmove(c.rx.data(),c.rx.data()+off,c.have-off);
        c.have-=off;
    }
    return result;
}

Receiver::Parse Receiver::handleMsg(Conn& c, const MsgHeader& h, const std::byte* body) {
    switch(h.type){
    case MsgType::HELLO:
        return handleHello(c,body,h.bodyLen);
    case MsgType::FRAGMENT:
        return handleFragment(c,body,h.bodyLen);
    case MsgType::BYE:
        c.bye=true;
        if(h.bodyLen==sizeof(ByeMsg)) c.byeCount=get<ByeMsg>(body).fragsSent;
        return Parse::OK;
    default:
        return Parse::ERROR;  //frontends never send CREDIT/START
    }
}

/*
source id ok + not taken?
new conn generation (old frags still in the pipeline wont give credits to this conn)
first time all sources are here -> start the run
run already going -> its a restart, just send START
*/
Receiver::Parse Receiver::handleHello(Conn& c, const std::byte* body, uint32_t len) {
    if(len!=sizeof(HelloMsg) || c.source>=0) return Parse::ERROR;
    auto hm=get<HelloMsg>(body);
    bool badId=hm.sourceId>=cfg.sources;
    bool taken=!badId && sourceFd[hm.sourceId]!=-1;
    if(badId || taken){
        std::fprintf(stderr,"builder: rejecting HELLO from source %u\n",hm.sourceId);
        return Parse::ERROR;
    }
    uint16_t src=hm.sourceId;
    c.source=src;
    sourceFd[src]=c.fd;
    SourceCredit& cr=shared.credit[src];
    c.gen=cr.gen.fetch_add(1,std::memory_order_acq_rel)+1;
    cr.released.store(0,std::memory_order_release);
    c.lastGrant=cfg.credits;
    cr.wakeAt.store(grantStep,std::memory_order_seq_cst);  //first poke after grantStep releases
    track[src].resetConn();

    uint64_t bit=1ull<<src;
    if(everConnected&bit){
        reconnects.add();
        std::fprintf(stderr,"builder: source %u reconnected\n",src);
    }
    everConnected|=bit;
    shared.liveMask.fetch_or(bit,std::memory_order_acq_rel);
    connected.add();
    hellos++;

    if(runStarted()){
        sendStart(c);
    }
    else if(hellos==cfg.sources){
        startRun();
    }
    return Parse::OK;
}

void Receiver::startRun() {
    t0=onStart();
    for(auto& [fd,cc]:conns){
        if(cc.source>=0) sendStart(cc);
    }
    if(!cfg.quiet) std::fprintf(stderr,"builder: all %u sources connected, run started\n",cfg.sources);
}

void Receiver::sendStart(Conn& c) {
    std::byte b[64];
    size_t n=encodeSimple(b,MsgType::START,StartMsg{t0,cfg.credits});
    queueSend(c,b,n);
}

/*
frag checks: from the right source, lengths add up, fits in a pool slot
get a slot first (none -> STALL, msg stays unread)
seq tracking -> dup / too old frags stop here
crc bad -> drop it here
copy payload into the slot, push to shard eventId % shards
*/
Receiver::Parse Receiver::handleFragment(Conn& c, const std::byte* body, uint32_t len) {
    if(c.source<0 || len<sizeof(FragmentHeader)) return Parse::ERROR;
    auto hdr=get<FragmentHeader>(body);
    bool lenOk=len==sizeof(FragmentHeader)+hdr.payloadLen;
    bool fits=hdr.payloadLen<=shared.pool.slotBytes();
    if(hdr.sourceId!=c.source || !lenOk || !fits) return Parse::ERROR;

    uint32_t buf=shared.pool.acquire();
    if(buf==BufferPool::NONE){
        poolEmpty.add();
        return Parse::STALL;
    }

    c.rxCount++;
    fragmentsRx.add();
    bytesRx.add(sizeof(MsgHeader)+len);

    SourceTracker& t=track[hdr.sourceId];
    uint64_t gapsBefore=t.seqGaps;
    uint64_t reordBefore=t.reordered;
    uint64_t skipsBefore=t.sourceSkips;
    auto verdict=t.observe(hdr.seq,hdr.eventId);
    seqGaps.add(t.seqGaps-gapsBefore);
    reordered.add(t.reordered-reordBefore);
    sourceSkips.add(t.sourceSkips-skipsBefore);

    bool dropIt=false;
    if(verdict==SourceTracker::Verdict::DUPLICATE){
        duplicatesRx.add();
        dropIt=true;
    }
    else if(verdict==SourceTracker::Verdict::TOO_OLD){
        tooOldRx.add();
        dropIt=true;
    }
    const std::byte* payload=body+sizeof(FragmentHeader);
    if(!dropIt && crc32c(payload,hdr.payloadLen)!=hdr.payloadCrc){
        crcErrors.add();
        dropIt=true;
    }
    if(dropIt){
        shared.pool.release(buf);
        shared.releaseCredit(hdr.sourceId,c.gen);
        return Parse::OK;
    }

    std::memcpy(shared.pool.data(buf),payload,hdr.payloadLen);
    Fragment f;
    f.hdr=hdr;
    f.buf=buf;
    f.gen=c.gen;
    f.recvTs=nowNs();
    Shard& shard=*shards[hdr.eventId%shards.size()];
    //inbox is sized so this shouldnt fail, if it does we spin (= head of line blocking, counted)
    while(!shard.inbox.tryPush(std::move(f))){
        inboxFullSpins.add();
        std::this_thread::yield();
    }
    return Parse::OK;
} /* Receiver::handleFragment() */

/*
per connected source: limit = released + credits
only send when it moved by at least grantStep (credits/8)
then tell the releasers at which count to poke us next (wakeAt).
if released already went past that while we were busy -> go again, else that poke is lost
*/
void Receiver::grantCredits() {
    for(auto& [fd,c]:conns){
        if(c.source<0) continue;
        SourceCredit& cr=shared.credit[c.source];
        while(true){
            uint64_t released=cr.released.load(std::memory_order_seq_cst);
            uint64_t limit=released+cfg.credits;
            if(limit>=c.lastGrant+grantStep){
                std::byte b[64];
                size_t n=encodeSimple(b,MsgType::CREDIT,CreditMsg{limit});
                queueSend(c,b,n);
                c.lastGrant=limit;
                creditMsgs.add();
            }
            uint64_t next=c.lastGrant+grantStep-cfg.credits;  //released value that allows the next grant
            cr.wakeAt.store(next,std::memory_order_seq_cst);
            bool missed=cr.released.load(std::memory_order_seq_cst)>=next;
            if(!missed) break;
        }
        if(!c.tx.empty()) flushTx(c);
    }
}

void Receiver::queueSend(Conn& c, const std::byte* p, size_t n) {
    c.tx.insert(c.tx.end(),p,p+n);
    flushTx(c);
}

//nonblocking, whatever doesnt fit stays in tx for next time (so a partial write cant corrupt the stream)
void Receiver::flushTx(Conn& c) {
    while(!c.tx.empty()){
        ssize_t n=send(c.fd,c.tx.data(),c.tx.size(),MSG_NOSIGNAL|MSG_DONTWAIT);
        if(n<=0) return;
        c.tx.erase(c.tx.begin(),c.tx.begin()+n);
    }
}

/*
source conn closing:
  no BYE -> producer failure
  BYE but count doesnt match -> smtg went missing in the stream (shouldnt happen w/ tcp)
  mark source dead (degraded mode uses this), clear its busy bit or the trigger stays vetoed
*/
void Receiver::closeConn(int fd) {
    auto it=conns.find(fd);
    if(it==conns.end()) return;
    Conn& c=it->second;
    if(c.source>=0){
        uint16_t src=static_cast<uint16_t>(c.source);
        if(!c.bye){
            producerFailures.add();
            std::fprintf(stderr,"builder: source %u disconnected without BYE (producer failure)\n",src);
        }
        else if(c.byeCount!=c.rxCount){
            byeMismatch.add();
        }
        shared.liveMask.fetch_and(~(1ull<<src),std::memory_order_acq_rel);
        ttc.clearBusy(src);
        sourceFd[src]=-1;
        connected.set(connected.get()-1);
    }
    epoll_ctl(ep,EPOLL_CTL_DEL,fd,nullptr);
    close(fd);
    conns.erase(it);
}

} // namespace daq
