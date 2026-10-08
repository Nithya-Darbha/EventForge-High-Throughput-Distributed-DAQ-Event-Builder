#include "builder/shard.hpp"

#include <bit>
#include <thread>

#include "daq/clock.hpp"
#include "daq/futex.hpp"

namespace daq {

//---------- ShardInbox ----------

ShardInbox::ShardInbox(bool spsc, size_t cap) {
    if(spsc){
        ring=std::make_unique<SpscRing<Fragment>>(cap);
    }
    else{
        q=std::make_unique<BoundedQueue<Fragment>>(cap);
    }
}

/*
spsc: push, bump pushSeq, if the shard is asleep wake it
the fence pairs with the one in popFor (Dekker: we store then load, it stores then loads)
*/
bool ShardInbox::tryPush(Fragment&& f) {
    if(q) return q->tryPush(std::move(f));
    if(!ring->tryPush(std::move(f))) return false;
    pushSeq.fetch_add(1,std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if(sleeping.load(std::memory_order_relaxed)){
        futexWake(&pushSeq);
    }
    return true;
}

bool ShardInbox::tryPop(Fragment& f) {
    return ring ? ring->tryPop(f) : q->tryPop(f);
}

/*
mutex queue -> just wait on its condvar
spsc ring   -> spin a bit first (frags usually come in bursts)
               then: say we're sleeping, check once more, sleep on the futex till a push
v1 slept with a 10..160us backoff instead. worked on linux, but in a mac VM every short
sleep took ~2.5ms -> p50 latency 5ms. a futex wakeup is an event, not a timer
*/
bool ShardInbox::popFor(Fragment& f, uint64_t timeoutNs) {
    if(q) return q->popFor(f,timeoutNs);
    for(int i=0;i<64;i++){
        if(ring->tryPop(f)) return true;
        cpuRelax();
    }
    uint64_t deadline=nowNs()+timeoutNs;
    while(true){
        uint32_t seen=pushSeq.load(std::memory_order_relaxed);
        sleeping.store(1,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        bool got=ring->tryPop(f);
        if(got || closed.load(std::memory_order_acquire)){
            sleeping.store(0,std::memory_order_relaxed);
            return got || ring->tryPop(f);
        }
        uint64_t now=nowNs();
        if(now>=deadline){
            sleeping.store(0,std::memory_order_relaxed);
            return false;
        }
        futexWait(&pushSeq,seen,deadline-now);
        sleeping.store(0,std::memory_order_relaxed);
        if(ring->tryPop(f)) return true;
    }
}

void ShardInbox::close() {
    closed.store(true,std::memory_order_release);
    if(q){
        q->close();
    }
    else{
        pushSeq.fetch_add(1,std::memory_order_seq_cst);
        futexWake(&pushSeq);
    }
}

bool ShardInbox::closedAndEmpty() const {
    return closed.load(std::memory_order_acquire) && size()==0;
}

size_t ShardInbox::size() const { return ring ? ring->size() : q->size(); }
size_t ShardInbox::capacity() const { return ring ? ring->capacity() : q->capacity(); }

//---------- Shard ----------

static size_t inboxCapacity(const BuilderConfig& cfg) {
    //every frag in flight could hash to one shard in theory, so sources*credits never fills up
    if(cfg.inboxCap) return cfg.inboxCap;
    return std::bit_ceil(static_cast<size_t>(cfg.poolSlots()));
}

Shard::Shard(uint32_t i, const BuilderConfig& c, Shared& s, BoundedQueue<Event>& d)
    : inbox(c.spscQueues,inboxCapacity(c)),
      idx(i), cfg(c), shared(s), done(d),
      asmb(c.sources,c.timeoutNs),
      sweeper(5'000'000,c.degraded,asmb.fullMask()) {}

void Shard::start() {
    th=std::thread([this]{ run(); });
}

void Shard::join() {
    if(th.joinable()) th.join();
}

/*
loop
  pop frag (1ms timeout so sweeps still happen when its quiet)
  got one -> add it, then grab whatever else is already queued (up to 64)
  closed + empty -> done
  sweeper tick
end: flush whats left as incomplete
*/
void Shard::run() {
    nameThread("eb-shard"+std::to_string(idx));
    Fragment f;
    while(true){
        bool got=inbox.popFor(f,1'000'000);
        uint64_t now=nowNs();
        if(got){
            handle(f,now);
            for(int i=0;i<64 && inbox.tryPop(f);i++){
                handle(f,now);
            }
        }
        else if(inbox.closedAndEmpty()){
            break;
        }
        sweeper.tick(now,asmb,shared.liveMask.load(std::memory_order_relaxed),
                     [this](Event&& ev){ emit(std::move(ev)); },
                     [this](Event&& ev){ dropEvent(std::move(ev)); });
        publishStats();
    }
    asmb.flush(nowNs(),[this](Event&& ev){ dropEvent(std::move(ev)); });
    publishStats();
} /* Shard::run() */

void Shard::handle(Fragment& f, uint64_t now) {
    uint64_t required=sweeper.requiredMask(shared.liveMask.load(std::memory_order_relaxed));
    Event ev;
    AddResult r=asmb.add(f,now,required,ev);
    switch(r){
    case AddResult::PENDING:
        break;
    case AddResult::COMPLETE:
        emit(std::move(ev));
        break;
    case AddResult::DUPLICATE:
    case AddResult::LATE:
    case AddResult::BAD_SOURCE:
        shared.releaseFrag(f);  //frag goes nowhere, give the slot+credit back now
        break;
    }
}

//blocking push = the backpressure point, time spent here = sinks are the bottleneck
void Shard::emit(Event&& ev) {
    uint64_t t=nowNs();
    bool ok=done.tryPush(std::move(ev));
    if(!ok){
        //ev wasnt moved from if tryPush failed
        ok=done.push(std::move(ev));
        doneBlockedNs.add(nowNs()-t);
    }
    if(!ok){
        //done queue closed (shouldnt happen, shards stop first) -> dont leak buffers
        for(auto& fr:ev.frags) shared.releaseFrag(fr);
    }
}

//incomplete event: nobody wants it, free everything
void Shard::dropEvent(Event&& ev) {
    for(auto& fr:ev.frags){
        shared.releaseFrag(fr);
    }
}

void Shard::publishStats() {
    const auto& s=asmb.stats();
    partials.set(asmb.partials());
    complete.set(s.eventsComplete);
    degraded.set(s.eventsDegraded);
    incomplete.set(s.eventsIncomplete);
    duplicates.set(s.duplicates);
    late.set(s.late);
}

} // namespace daq
