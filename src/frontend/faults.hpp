#pragma once
/*
 * FaultInjector: controlled faults on the frontend side, all seeded so a run can be repeated.
 *
 *  loss     frag gets a seq but never goes on the wire  -> builder sees a seq gap
 *  dup      frag is sent twice                          -> builder sees the same seq twice
 *  reorder  frag is held back and sent after the next   -> seq goes backwards once
 *  corrupt  one payload byte flipped after the crc      -> builder crc check fails
 *  die      process _exit()s at a given time, no Bye    -> producer failure
 *  stall    frontend freezes for a while                -> stuck readout board
 *
 * producer kill/restart from the outside (SIGKILL) is done by experiments/run.py
 */
#include <cstdint>
#include <random>

#include "daq/args.hpp"

namespace daq {

struct FaultConfig {
    double lossProb=0;
    double dupProb=0;
    double reorderProb=0;
    double corruptProb=0;
    double dieAtS=-1;      //<0 = never
    double stallAtS=-1;
    double stallForS=0;
    uint64_t seed=1;

    static FaultConfig fromArgs(const Args& a);
};

class FaultInjector {
public:
    struct Decision {
        bool lose=false;
        bool duplicate=false;
        bool reorder=false;
        bool corrupt=false;
    };

    FaultInjector(const FaultConfig& cfg, uint16_t sourceId);

    Decision roll();
    bool shouldDie(uint64_t now, uint64_t t0) const;
    uint64_t stallFor(uint64_t now, uint64_t t0);  //>0 once, when its time to stall

    bool anyEnabled() const;

    //what we actually injected, so tests can check the builder counted the same
    uint64_t lost=0;
    uint64_t dups=0;
    uint64_t reorders=0;
    uint64_t corrupts=0;
    uint64_t stalls=0;

private:
    FaultConfig cfg;
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> uni{0.0,1.0};
    bool stalled=false;
};

} // namespace daq
