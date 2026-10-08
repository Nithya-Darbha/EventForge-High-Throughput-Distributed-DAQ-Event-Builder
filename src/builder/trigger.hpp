#pragma once
/*
 * TriggerGen: the central trigger, a thread in the builder process that writes
 * triggers into the TTC shared memory at the configured rate.
 *
 * rate can change over the run (bursts / temporary overload) with a profile:
 *   "0:10000,2:40000,2.5:10000" = 10 kHz, at t=2s 40 kHz, at t=2.5s back to 10 kHz
 *
 * at every trigger time: any frontend busy -> VETO (counted as deadtime), else publish.
 * triggers are issued in small batches (default every 20us) so frontends get one
 * wakeup per batch instead of one per trigger. timestamps are still the exact ones.
 */
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "daq/metrics.hpp"
#include "daq/ttc.hpp"

namespace daq {

class RateProfile {
public:
    static RateProfile parse(const std::string& spec, double defaultRate);
    double rateAt(double tS) const;  //Hz at t seconds into the run

private:
    std::vector<std::pair<double, double>> points;  //(t, rate), sorted by t
};

class TriggerGen {
public:
    TriggerGen(TtcHost& ttc, RateProfile profile, double durationS, uint64_t batchNs);
    ~TriggerGen();

    void start(uint64_t t0);
    void join();
    void requestStop() { stopReq.store(true,std::memory_order_release); }  //ctrl-c
    bool finished() const { return done.load(std::memory_order_acquire); }
    bool started() const { return t0.load(std::memory_order_acquire)!=0; }
    uint64_t startTs() const { return t0.load(std::memory_order_acquire); }

    Counter issued;
    Counter vetoed;

private:
    void run();

    TtcHost& ttc;
    RateProfile profile;
    uint64_t durationNs;
    uint64_t batchNs;
    std::atomic<uint64_t> t0{0};  //sampler thread reads it too
    std::atomic<bool> done{false};
    std::atomic<bool> stopReq{false};
    std::thread th;
};

} // namespace daq
