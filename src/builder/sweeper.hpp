#pragma once
/*
 * Sweeper: the housekeeping a shard does on a timer, between fragments
 *  - evict partial events older than the timeout (they go out as incomplete)
 *  - degraded mode: when a source dies, complete the events that were only
 *    waiting for it instead of letting them all time out
 */
#include <cstdint>

#include "builder/assembler.hpp"

namespace daq {

class Sweeper {
public:
    Sweeper(uint64_t intervalNs, bool degraded, uint64_t fullMask);

    //call as often as you like, only does work every intervalNs
    void tick(uint64_t now, Assembler& a, uint64_t liveMask,
              const Assembler::EventFn& onComplete, const Assembler::EventFn& onIncomplete);

    uint64_t requiredMask(uint64_t liveMask) const;
    uint64_t sweeps() const { return sweepCnt; }

private:
    uint64_t interval;
    bool degraded;
    uint64_t full;
    uint64_t last=0;
    uint64_t lastLive=0;
    uint64_t sweepCnt=0;
};

} // namespace daq
