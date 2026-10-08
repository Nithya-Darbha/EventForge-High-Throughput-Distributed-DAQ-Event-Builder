#include "frontend/faults.hpp"

#include "daq/clock.hpp"

namespace daq {

FaultConfig FaultConfig::fromArgs(const Args& a) {
    FaultConfig c;
    c.lossProb=a.f64("loss",0);
    c.dupProb=a.f64("dup",0);
    c.reorderProb=a.f64("reorder",0);
    c.corruptProb=a.f64("corrupt",0);
    c.dieAtS=a.f64("die-at-s",-1);
    c.stallAtS=a.f64("stall-at-s",-1);
    c.stallForS=a.f64("stall-for-s",0);
    c.seed=a.u64("seed",1);
    return c;
}

FaultInjector::FaultInjector(const FaultConfig& c, uint16_t sourceId)
    : cfg(c), rng(c.seed*1000003ull+sourceId) {}

bool FaultInjector::anyEnabled() const {
    return cfg.lossProb>0 || cfg.dupProb>0 || cfg.reorderProb>0 || cfg.corruptProb>0 ||
           cfg.dieAtS>=0 || cfg.stallAtS>=0;
}

/*
one roll per frag
lose wins over everything (a lost frag cant also be duped)
otherwise dup / reorder / corrupt independently
*/
FaultInjector::Decision FaultInjector::roll() {
    Decision d;
    if(cfg.lossProb>0 && uni(rng)<cfg.lossProb){
        d.lose=true;
        lost++;
        return d;
    }
    if(cfg.corruptProb>0 && uni(rng)<cfg.corruptProb){
        d.corrupt=true;
        corrupts++;
    }
    //dups/reorders are counted by the caller when it actually does them
    //(cant hold a 2nd frag back while one is already held, etc)
    if(cfg.dupProb>0 && uni(rng)<cfg.dupProb){
        d.duplicate=true;
    }
    if(cfg.reorderProb>0 && uni(rng)<cfg.reorderProb){
        d.reorder=true;
    }
    return d;
}

bool FaultInjector::shouldDie(uint64_t now, uint64_t t0) const {
    if(cfg.dieAtS<0 || now<t0) return false;
    return nsToS(now-t0)>=cfg.dieAtS;
}

uint64_t FaultInjector::stallFor(uint64_t now, uint64_t t0) {
    if(stalled || cfg.stallAtS<0 || now<t0) return 0;
    if(nsToS(now-t0)<cfg.stallAtS) return 0;
    stalled=true;
    stalls++;
    return static_cast<uint64_t>(cfg.stallForS*1e9);
}

} // namespace daq
