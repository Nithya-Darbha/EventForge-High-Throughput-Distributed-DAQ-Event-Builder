#pragma once
/*
 * Shard: one assembler thread. Events are spread over shards by eventId % shards,
 * the receiver pushes each frag into the right shard's inbox. A shard owns its
 * Assembler completely so there is no locking on the event map.
 *
 * complete event -> pushed into the done queue for the sinks. That push BLOCKS when the
 * sinks are behind -> shard stops pulling frags -> buffers + credits stay used ->
 * frontends run out of credits. Thats the backpressure chain.
 */
#include <memory>
#include <thread>

#include "builder/assembler.hpp"
#include "builder/builder.hpp"
#include "builder/sweeper.hpp"
#include "daq/bounded_queue.hpp"
#include "daq/metrics.hpp"

namespace daq {

class Shard {
public:
    Shard(uint32_t idx, const BuilderConfig& cfg, Shared& shared, BoundedQueue<Event>& done);

    void start();
    void join();

    ShardInbox inbox;

    //for the sampler (copied out of the assembler stats every loop)
    Counter partials;
    Counter complete;
    Counter degraded;
    Counter incomplete;
    Counter duplicates;
    Counter late;
    Counter doneBlockedNs;  //time spent stuck pushing into a full done queue

    //only read these after join()
    const Assembler& assembler() const { return asmb; }

private:
    void run();
    void handle(Fragment& f, uint64_t now);
    void emit(Event&& ev);
    void dropEvent(Event&& ev);
    void publishStats();

    uint32_t idx;
    const BuilderConfig& cfg;
    Shared& shared;
    BoundedQueue<Event>& done;
    Assembler asmb;
    Sweeper sweeper;
    std::thread th;
};

} // namespace daq
