#include "daq/clock.hpp"

#include <algorithm>
#include <atomic>
#include <vector>

namespace daq {
namespace {

std::atomic<uint64_t> spinThreshold{0};
std::atomic<uint64_t> measuredOvershoot{0};

} // namespace

void setSpinNs(uint64_t ns) { spinThreshold.store(ns,std::memory_order_relaxed); }
uint64_t spinNs() { return spinThreshold.load(std::memory_order_relaxed); }
uint64_t sleepOvershootNs() { return measuredOvershoot.load(std::memory_order_relaxed); }

/*
100x sleep 50us, how late do we wake up
spin = p90 overshoot * 1.5 + 10us, max 5ms
*/
uint64_t calibrateSpin() {
    tightTimerSlack();
    const uint64_t want=50*NS_PER_US;
    std::vector<uint64_t> late;
    for(int i=0;i<100;i++){
        uint64_t t=nowNs();
        sleepUntilNs(t+want);
        uint64_t took=nowNs()-t;
        late.push_back(took>want ? took-want : 0);
    }
    std::sort(late.begin(),late.end());
    uint64_t p90=late[90];
    measuredOvershoot.store(p90,std::memory_order_relaxed);
    uint64_t spin=std::min<uint64_t>(p90+p90/2+10*NS_PER_US,5*NS_PER_MS);
    setSpinNs(spin);
    return spin;
}

void preciseSleepUntil(uint64_t deadline) {
    uint64_t spin=spinNs();
    uint64_t now=nowNs();
    if(deadline>now+spin){
        sleepUntilNs(deadline-spin);
    }
    while(nowNs()<deadline){
        cpuRelax();
    }
}

} // namespace daq
