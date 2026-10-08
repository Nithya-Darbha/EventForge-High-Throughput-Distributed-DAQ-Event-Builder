#pragma once
/*
 * SinkPool: worker threads at the end of the pipeline ("processing / storage").
 * Pop a complete event from the done queue, pretend to store it (configurable delay,
 * optional payload check), record latency, give buffers + credits back.
 *
 * latency = sink commit time - trigger time, i.e. trigger -> event safely stored.
 * each worker records into its own histogram (mutex only fights with the sampler).
 */
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "builder/assembler.hpp"
#include "builder/builder.hpp"
#include "daq/bounded_queue.hpp"
#include "daq/histogram.hpp"
#include "daq/metrics.hpp"

namespace daq {

class SinkPool {
public:
    SinkPool(const BuilderConfig& cfg, Shared& shared, BoundedQueue<Event>& done);

    void start();
    void setRunStart(uint64_t t0);  //for the slowdown window
    void join();

    //merged interval histogram since the last call, then resets (sampler uses it)
    Histogram takeInterval();
    //totals, after join
    Histogram totalE2e() const;
    Histogram totalBuild() const;

    Counter events;
    Counter bytes;
    Counter degraded;
    Counter verifyFail;
    Counter lastCommitTs;

private:
    struct Worker {
        std::thread th;
        mutable std::mutex mtx;
        Histogram interval;
        Histogram e2e;
        Histogram build;  //first frag arrived -> event complete
    };

    void run(Worker& w, uint32_t idx);
    uint64_t delayFor(uint64_t now) const;

    const BuilderConfig& cfg;
    Shared& shared;
    BoundedQueue<Event>& done;
    std::vector<std::unique_ptr<Worker>> workers;
    std::atomic<uint64_t> t0{0};
};

} // namespace daq
