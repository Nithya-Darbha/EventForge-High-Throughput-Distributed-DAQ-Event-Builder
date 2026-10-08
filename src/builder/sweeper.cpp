#include "builder/sweeper.hpp"

namespace daq {

Sweeper::Sweeper(uint64_t intervalNs, bool deg, uint64_t fullMask)
    : interval(intervalNs), degraded(deg), full(fullMask), lastLive(fullMask) {}

//normal mode: always need every source. degraded: only the ones that are alive
uint64_t Sweeper::requiredMask(uint64_t liveMask) const {
    return degraded ? (full&liveMask) : full;
}

/*
not time yet -> nothing
degraded + live mask lost a source since last time -> complete what only waited for it
then timeouts
*/
void Sweeper::tick(uint64_t now, Assembler& a, uint64_t liveMask,
                   const Assembler::EventFn& onComplete, const Assembler::EventFn& onIncomplete) {
    if(now-last<interval) return;
    last=now;
    sweepCnt++;

    bool lostSource=(lastLive&~liveMask)!=0;
    if(degraded && lostSource){
        a.completeReady(requiredMask(liveMask),now,onComplete);
    }
    lastLive=liveMask;

    a.sweep(now,onIncomplete);
}

} // namespace daq
