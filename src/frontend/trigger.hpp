#pragma once
/*
 * TriggerInput: the frontend's side of the TTC.
 * Wraps TtcClient and decides when this frontend raises BUSY.
 *
 * busy (only with throttle on = BLOCK policy):
 *   set   when out of credits OR backlog >= derandDepth   (derandomizer buffer "full")
 *   clear when we have credits AND backlog < derandDepth/2 (hysteresis so it doesnt flap)
 */
#include <cstdint>
#include <string>

#include "daq/ttc.hpp"

namespace daq {

class TriggerInput {
public:
    TriggerInput(const std::string& shmName, uint16_t sourceId, bool throttle, uint64_t derandDepth);
    ~TriggerInput();

    bool next(uint64_t& eventId, uint64_t& triggerTs);  //false = run is over
    void updateBusy(bool hasCredit);
    void dropBusy();  //end of run, never leave our bit set

    uint64_t backlog() const { return ttc.backlog(); }
    uint64_t overruns() const { return ttc.overruns(); }
    uint64_t busyNs() const;
    uint64_t busyCount() const { return busyAsserts; }
    uint64_t maxBacklog() const { return maxLag; }

private:
    void setBusy(bool b);

    TtcClient ttc;
    uint16_t sourceId;
    bool throttle;
    uint64_t derandDepth;
    bool busy=false;
    uint64_t busySince=0;
    uint64_t busyTotal=0;
    uint64_t busyAsserts=0;
    uint64_t maxLag=0;
};

} // namespace daq
