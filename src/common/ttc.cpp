#include "daq/ttc.hpp"

#include "daq/futex.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>

namespace daq {
namespace {

std::runtime_error sysError(const std::string& what) {
    return std::runtime_error(what+": "+std::strerror(errno));
}

} // namespace

std::string ttcName(uint16_t port) {
    return "/eventforge_"+std::to_string(port);
}

/*
unlink leftovers from a crashed run
create, size it, map it
placement new so all the atomics get initialised
magic last -> clients know its ready
*/
TtcHost::TtcHost(const std::string& n) : name(n) {
    shm_unlink(name.c_str());
    int fd=shm_open(name.c_str(),O_CREAT|O_EXCL|O_RDWR,0600);
    if(fd<0) throw sysError("shm_open "+name);
    if(ftruncate(fd,sizeof(TtcShared))<0){
        close(fd);
        throw sysError("ftruncate");
    }
    void* p=mmap(nullptr,sizeof(TtcShared),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(p==MAP_FAILED) throw sysError("mmap");
    mem=new (p) TtcShared();
    mem->magic.store(TTC_MAGIC,std::memory_order_release);
}

TtcHost::~TtcHost() {
    if(mem){
        mem->~TtcShared();
        munmap(mem,sizeof(TtcShared));
    }
    shm_unlink(name.c_str());
}

/*
seqlock write of one slot:
id=INVALID (readers will reject the slot), write ts, id=new id (release)
then bump published so readers know its there
*/
void TtcHost::publish(uint64_t triggerTs) {
    uint64_t id=mem->published.load(std::memory_order_relaxed);
    TtcSlot& s=mem->ring[id&(TTC_RING-1)];
    s.id.store(TTC_INVALID,std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.ts.store(triggerTs,std::memory_order_relaxed);
    s.id.store(id,std::memory_order_release);
    mem->published.store(id+1,std::memory_order_release);
}

/*
bump wakeSeq, then check if anyone sleeps. client does the mirror image (waiters++ then
sleep on wakeSeq). store->load on both sides = Dekker, needs a full fence each side or on
ARM both loads can see the old value -> missed wakeup (x86 hides this, M1 doesnt)
*/
void TtcHost::wakeAll() {
    mem->wakeSeq.fetch_add(1,std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    //skip the syscall when nobody sleeps
    if(mem->waiters.load(std::memory_order_relaxed)>0){
        futexWake(&mem->wakeSeq,true);  //shared: sleepers are other processes
    }
}

void TtcHost::setRunState(RunState s) {
    mem->runState.store(static_cast<uint32_t>(s),std::memory_order_release);
    wakeAll();
}

void TtcHost::clearBusy(uint16_t sourceId) {
    mem->busyMask.fetch_and(~(1ull<<sourceId),std::memory_order_acq_rel);
}

/*
segment might not exist yet / not initialised yet -> retry for timeoutMs
*/
TtcClient::TtcClient(const std::string& name, int timeoutMs) {
    for(int waited=0;;waited+=10){
        int fd=shm_open(name.c_str(),O_RDWR,0);
        if(fd>=0){
            void* p=mmap(nullptr,sizeof(TtcShared),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
            close(fd);
            if(p==MAP_FAILED) throw sysError("mmap");
            auto* candidate=static_cast<TtcShared*>(p);
            if(candidate->magic.load(std::memory_order_acquire)==TTC_MAGIC){
                mem=candidate;
                return;
            }
            munmap(p,sizeof(TtcShared));
        }
        if(waited>=timeoutMs) throw std::runtime_error("ttc segment "+name+" never showed up");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

TtcClient::~TtcClient() {
    if(mem) munmap(mem,sizeof(TtcShared));
}

void TtcClient::joinNow() {
    next=mem->published.load(std::memory_order_acquire);
}

//seqlock read: id before and after has to be the one we want, else the slot got reused
bool TtcClient::readSlot(uint64_t id, uint64_t& ts) const {
    const TtcSlot& s=mem->ring[id&(TTC_RING-1)];
    uint64_t before=s.id.load(std::memory_order_acquire);
    if(before!=id) return false;
    ts=s.ts.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t after=s.id.load(std::memory_order_relaxed);
    return after==id;
}

/*
loop
  smtg published we havent read -> read slot
     slot overwritten (we lagged > ring) -> count overrun, skip ahead
  run stopped + nothing left -> END
  else sleep on the futex (2ms timeout just in case)
*/
TtcClient::Result TtcClient::waitNext(uint64_t& eventId, uint64_t& triggerTs) {
    while(true){
        uint64_t pub=mem->published.load(std::memory_order_acquire);
        if(next<pub){
            uint64_t ts=0;
            if(readSlot(next,ts)){
                eventId=next;
                triggerTs=ts;
                next++;
                return Result::GOT;
            }
            //lagged more than a ring: jump to half a ring behind the newest (not the very
            //oldest slot, the generator might be overwriting that one right now)
            uint64_t resume=pub>TTC_RING/2 ? pub-TTC_RING/2 : pub;
            if(resume<=next) resume=pub;
            overrunCnt+=resume-next;
            next=resume;
            continue;
        }

        bool stopped=mem->runState.load(std::memory_order_acquire)==static_cast<uint32_t>(RunState::STOPPED);
        if(stopped){
            //generator may have published right before stopping, check once more
            if(next>=mem->published.load(std::memory_order_acquire)) return Result::END;
            continue;
        }

        uint32_t w=mem->wakeSeq.load(std::memory_order_acquire);
        if(mem->published.load(std::memory_order_acquire)!=pub) continue;  //raced w/ a publish
        mem->waiters.fetch_add(1,std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        futexWait(&mem->wakeSeq,w,2'000'000,true);
        mem->waiters.fetch_sub(1,std::memory_order_relaxed);
    }
} /* TtcClient::waitNext() */

uint64_t TtcClient::backlog() const {
    uint64_t pub=mem->published.load(std::memory_order_acquire);
    return pub>next ? pub-next : 0;
}

void TtcClient::setBusy(uint16_t sourceId, bool busy) {
    uint64_t bit=1ull<<sourceId;
    if(busy){
        mem->busyMask.fetch_or(bit,std::memory_order_acq_rel);
    }
    else{
        mem->busyMask.fetch_and(~bit,std::memory_order_acq_rel);
    }
}

RunState TtcClient::runState() const {
    return static_cast<RunState>(mem->runState.load(std::memory_order_acquire));
}

} // namespace daq
