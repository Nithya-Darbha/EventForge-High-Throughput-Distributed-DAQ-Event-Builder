/*
 * Frontend = one simulated detector readout board.
 *
 * gets triggers from the TTC (shared mem), makes one fragment per trigger and sends it
 * to the builder over tcp. Can only send while it has credits from the builder.
 * Out of credits:
 *   BLOCK -> raise BUSY (trigger gets vetoed = deadtime) and wait for credits, nothing lost
 *   DROP  -> throw the fragment away and carry on, no deadtime but incomplete events
 */
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "daq/args.hpp"
#include "daq/clock.hpp"
#include "daq/crc32c.hpp"
#include "daq/metrics.hpp"
#include "daq/net.hpp"
#include "daq/payload.hpp"
#include "daq/ttc.hpp"
#include "daq/wire.hpp"
#include "frontend/faults.hpp"
#include "frontend/trigger.hpp"

using namespace daq;

namespace {

enum class Policy { BLOCK, DROP };

/*
reads CREDIT msgs coming back from the builder
credits are cumulative so we just keep the biggest limit we saw
*/
class CreditReader {
public:
    CreditReader(int sock, uint64_t initial) : fd(sock), limit(initial) {}

    //false = builder closed the conn. timeout 0 = just grab whats there, no poll() syscall
    bool poll(int timeoutMs) {
        if(timeoutMs>0){
            pollfd p{fd,POLLIN,0};
            int r=::poll(&p,1,timeoutMs);
            if(r<=0) return true;
        }
        while(true){
            ssize_t n=recv(fd,buf+have,sizeof(buf)-have,MSG_DONTWAIT);
            if(n==0) return false;
            if(n<0) return errno==EAGAIN || errno==EINTR;
            have+=static_cast<size_t>(n);
            parse();
        }
    }

    uint64_t creditLimit() const { return limit; }

private:
    void parse() {
        size_t off=0;
        size_t msgLen=sizeof(MsgHeader)+sizeof(CreditMsg);
        while(have-off>=msgLen){
            auto h=get<MsgHeader>(buf+off);
            if(validate(h)==HeaderCheck::OK && h.type==MsgType::CREDIT){
                auto c=get<CreditMsg>(buf+off+sizeof(MsgHeader));
                if(c.creditLimit>limit) limit=c.creditLimit;
            }
            off+=msgLen;
        }
        std::memmove(buf,buf+off,have-off);
        have-=off;
    }

    int fd;
    uint64_t limit;
    std::byte buf[512];
    size_t have=0;
};

} // namespace

int main(int argc, char** argv) {
    Args a(argc,argv);
    const auto sourceId=static_cast<uint16_t>(a.u64("source-id",0));
    const std::string host=a.str("host","127.0.0.1");
    const auto port=static_cast<uint16_t>(a.u64("port",9000));
    const auto pmin=static_cast<uint32_t>(a.u64("payload-min",4096));
    const auto pmax=static_cast<uint32_t>(a.u64("payload-max",pmin));
    const Policy policy=a.str("policy","block")=="drop" ? Policy::DROP : Policy::BLOCK;
    const uint64_t derand=a.u64("derand",64);
    const bool debug=a.flag("debug");  //print stuff for testing
    FaultInjector faults(FaultConfig::fromArgs(a),sourceId);

    if(sourceId>=64 || pmax<pmin || pmax+sizeof(FragmentHeader)>MAX_BODY_LEN){
        std::fprintf(stderr,"frontend: bad args\n");
        return 2;
    }
    tightTimerSlack();

    //connect + HELLO
    int fd=net::connectTcp(host,port);
    net::setNoDelay(fd);
    net::setSndBuf(fd,4<<20);
    std::byte small[64];
    size_t n=encodeSimple(small,MsgType::HELLO,HelloMsg{sourceId,0,static_cast<uint32_t>(getpid())});
    if(!net::writeAll(fd,small,n)) return 1;

    //wait for START, builder sends it once every source is connected
    if(!net::readAll(fd,small,sizeof(MsgHeader)+sizeof(StartMsg))){
        std::fprintf(stderr,"frontend %u: builder closed before START (source id taken?)\n",sourceId);
        return 1;
    }
    auto sh=get<MsgHeader>(small);
    if(validate(sh)!=HeaderCheck::OK || sh.type!=MsgType::START){
        std::fprintf(stderr,"frontend %u: expected START\n",sourceId);
        return 1;
    }
    auto start=get<StartMsg>(small+sizeof(MsgHeader));
    const uint64_t t0=start.t0;
    CreditReader credits(fd,start.creditLimit);
    TriggerInput trig(ttcName(port),sourceId,policy==Policy::BLOCK,derand);
    if(debug) std::fprintf(stderr,"frontend %u: start t0=%lu credits=%lu\n",sourceId,t0,start.creditLimit);

    std::mt19937_64 rng(payloadSeed(sourceId,0xABCDEF));
    std::uniform_int_distribution<uint32_t> psize(pmin,pmax);
    std::vector<std::byte> frag(fragWireSize(pmax));
    std::vector<std::byte> held(fragWireSize(pmax));  //for the reorder fault
    size_t heldLen=0;

    uint32_t seq=0;
    uint64_t sentWire=0;   //msgs actually on the wire, this is what credits count
    uint64_t triggers=0;
    uint64_t dropped=0;    //no credit + DROP policy
    uint64_t blockedNs=0;
    uint64_t bytes=0;
    uint64_t firstEvent=UINT64_MAX;
    bool builderGone=false;

    auto sendBuf=[&](const std::byte* p, size_t len) {
        if(!net::writeAll(fd,p,len)){
            builderGone=true;
            return;
        }
        sentWire++;
        bytes+=len;
    };

    /*
    loop
      wait for trigger (END -> run over)
      faults: die / stall
      read credits, update busy
      no credit -> DROP: skip it / BLOCK: wait till credit comes back
      build frag + crc, roll faults, send
    */
    uint64_t eventId=0;
    uint64_t triggerTs=0;
    while(!builderGone && trig.next(eventId,triggerTs)){
        triggers++;
        if(firstEvent==UINT64_MAX) firstEvent=eventId;
        uint64_t now=nowNs();

        if(faults.shouldDie(now,t0)){
            //crash on purpose: no Bye, no cleanup, kernel closes the socket
            std::fprintf(stderr,"frontend %u: dying on purpose at event %lu\n",sourceId,eventId);
            _exit(3);
        }
        if(uint64_t s=faults.stallFor(now,t0)){
            if(debug) std::fprintf(stderr,"frontend %u: stalling %.2fs\n",sourceId,nsToS(s));
            sleepNs(s);
        }

        if(!credits.poll(0)) break;
        bool hasCredit=sentWire<credits.creditLimit();
        trig.updateBusy(hasCredit);

        if(!hasCredit){
            if(policy==Policy::DROP){
                dropped++;
                continue;  //no seq for it -> builder sees an event id jump, not a seq gap
            }
            //BLOCK: busy is up now, so no new triggers while we wait
            uint64_t waitStart=nowNs();
            while(sentWire>=credits.creditLimit()){
                if(!credits.poll(1)){
                    builderGone=true;
                    break;
                }
            }
            blockedNs+=nowNs()-waitStart;
            if(builderGone) break;
            trig.updateBusy(true);
        }

        //build the fragment
        const uint32_t plen=psize(rng);
        std::byte* payload=frag.data()+sizeof(MsgHeader)+sizeof(FragmentHeader);
        fillPayload(sourceId,eventId,{payload,plen});
        FragmentHeader fh{};
        fh.eventId=eventId;
        fh.triggerTs=triggerTs;
        fh.sourceId=sourceId;
        fh.seq=seq++;
        fh.payloadLen=plen;
        fh.payloadCrc=crc32c(payload,plen);
        fh.sendTs=nowNs();
        put(frag.data(),makeHeader(MsgType::FRAGMENT,static_cast<uint32_t>(sizeof(FragmentHeader)+plen)));
        put(frag.data()+sizeof(MsgHeader),fh);
        const size_t wireLen=fragWireSize(plen);

        auto d=faults.roll();
        if(d.lose){
            continue;  //had a seq, never sent -> "lost in transit"
        }
        if(d.corrupt){
            payload[plen/2]^=std::byte{0x40};  //after the crc was computed
        }
        bool holdIt=d.reorder && heldLen==0;
        if(holdIt){
            //send it after the next frag instead
            std::memcpy(held.data(),frag.data(),wireLen);
            heldLen=wireLen;
            continue;
        }

        sendBuf(frag.data(),wireLen);
        if(heldLen>0){
            sendBuf(held.data(),heldLen);  //older seq after a newer one = reordered
            heldLen=0;
            faults.reorders++;
        }
        if(d.duplicate){
            sendBuf(frag.data(),wireLen);
            faults.dups++;
        }
    } /* while trigger */

    if(heldLen>0 && !builderGone) sendBuf(held.data(),heldLen);
    trig.dropBusy();

    //BYE, then wait for the builder to close so the Bye cant get lost to a RST
    n=encodeSimple(small,MsgType::BYE,ByeMsg{sentWire});
    net::writeAll(fd,small,n);
    shutdown(fd,SHUT_WR);
    while(recv(fd,small,sizeof(small),0)>0){
    }
    close(fd);

    JsonLine j;
    j.add("source_id",static_cast<uint64_t>(sourceId))
     .add("policy",std::string(policy==Policy::BLOCK ? "block" : "drop"))
     .add("first_event",firstEvent==UINT64_MAX ? 0 : firstEvent)
     .add("triggers",triggers)
     .add("sent",sentWire)
     .add("bytes",bytes)
     .add("dropped_no_credit",dropped)
     .add("blocked_ms",static_cast<double>(blockedNs)/1e6)
     .add("busy_ms",static_cast<double>(trig.busyNs())/1e6)
     .add("busy_asserts",trig.busyCount())
     .add("max_backlog",trig.maxBacklog())
     .add("ttc_overruns",trig.overruns())
     .add("lost_injected",faults.lost)
     .add("dup_injected",faults.dups)
     .add("reorder_injected",faults.reorders)
     .add("corrupt_injected",faults.corrupts)
     .add("stalls",faults.stalls)
     .add("cpu_s",processCpuSeconds());
    std::fprintf(stderr,"%s\n",j.str().c_str());
    return builderGone ? 1 : 0;
}
