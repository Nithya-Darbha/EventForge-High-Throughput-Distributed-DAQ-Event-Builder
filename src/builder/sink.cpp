#include "builder/sink.hpp"

#include <span>

#include "daq/clock.hpp"
#include "daq/payload.hpp"

namespace daq {

SinkPool::SinkPool(const BuilderConfig& c, Shared& s, BoundedQueue<Event>& d)
    : cfg(c), shared(s), done(d) {
    for(uint32_t i=0;i<c.sinks;i++){
        workers.push_back(std::make_unique<Worker>());
    }
}

void SinkPool::setRunStart(uint64_t startTs) {
    t0.store(startTs,std::memory_order_relaxed);
}

void SinkPool::start() {
    for(uint32_t i=0;i<workers.size();i++){
        Worker& w=*workers[i];
        w.th=std::thread([this,&w,i]{ run(w,i); });
    }
}

void SinkPool::join() {
    for(auto& w:workers){
        if(w->th.joinable()) w->th.join();
    }
}

//consumer slowdown fault: inside the window use slowDelay instead of the normal delay
uint64_t SinkPool::delayFor(uint64_t now) const {
    uint64_t start=t0.load(std::memory_order_relaxed);
    if(cfg.slowAtS>=0 && start && now>start){
        double t=nsToS(now-start);
        bool inWindow=t>=cfg.slowAtS && t<cfg.slowAtS+cfg.slowForS;
        if(inWindow) return cfg.slowDelayNs;
    }
    return cfg.sinkDelayNs;
}

/*
pop event (blocks, false = queue closed + empty -> exit)
"store" it: sleep the delay, check payload if --verify
record latency
free all frags -> slots + credits go back
*/
void SinkPool::run(Worker& w, uint32_t idx) {
    nameThread("eb-sink"+std::to_string(idx));
    tightTimerSlack();
    Event ev;
    while(done.pop(ev)){
        uint64_t delay=delayFor(nowNs());
        if(delay) sleepNs(delay);

        if(cfg.verify){
            for(const auto& f:ev.frags){
                std::span<const std::byte> payload{shared.pool.data(f.buf),f.hdr.payloadLen};
                if(!verifyPayload(f.hdr.sourceId,f.hdr.eventId,payload)) verifyFail.add();
            }
        }

        uint64_t commit=nowNs();
        {
            std::lock_guard<std::mutex> lk(w.mtx);
            w.interval.record(commit-ev.triggerTs);
            w.e2e.record(commit-ev.triggerTs);
            w.build.record(ev.doneTs-ev.firstArrival);
        }
        events.add();
        bytes.add(ev.bytes);
        if(ev.degraded) degraded.add();
        lastCommitTs.set(commit);

        for(const auto& f:ev.frags){
            shared.releaseFrag(f);
        }
        ev.frags.clear();
    }
} /* SinkPool::run() */

Histogram SinkPool::takeInterval() {
    Histogram h;
    for(auto& w:workers){
        std::lock_guard<std::mutex> lk(w->mtx);
        h.merge(w->interval);
        w->interval.reset();
    }
    return h;
}

Histogram SinkPool::totalE2e() const {
    Histogram h;
    for(const auto& w:workers){
        std::lock_guard<std::mutex> lk(w->mtx);
        h.merge(w->e2e);
    }
    return h;
}

Histogram SinkPool::totalBuild() const {
    Histogram h;
    for(const auto& w:workers){
        std::lock_guard<std::mutex> lk(w->mtx);
        h.merge(w->build);
    }
    return h;
}

} // namespace daq
