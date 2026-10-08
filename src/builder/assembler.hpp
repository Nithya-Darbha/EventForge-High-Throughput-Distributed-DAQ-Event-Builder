#pragma once
/*
 * Assembler: collects fragments by eventId until every required source has one.
 * Pure logic, no sockets, no threads, no clock (caller passes `now`) -> every
 * edge case (dup, late, timeout, degraded) is unit testable.
 * Each assembler shard thread owns one of these, so no locking inside.
 */
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include "daq/wire.hpp"

namespace daq {

struct Fragment {
    FragmentHeader hdr{};
    uint32_t buf=UINT32_MAX;  //slot in the BufferPool holding the payload
    uint32_t gen=0;           //conn generation of the source, for credit accounting
    uint64_t recvTs=0;
};

struct Event {
    uint64_t eventId=0;
    uint64_t triggerTs=0;
    uint64_t firstArrival=0;
    uint64_t doneTs=0;        //completed or evicted at
    uint64_t presentMask=0;   //bit per source that sent a frag
    bool complete=false;
    bool degraded=false;      //completed without a dead source (degraded mode)
    uint64_t bytes=0;
    std::vector<Fragment> frags;
};

struct AssemblerStats {
    uint64_t fragmentsIn=0;
    uint64_t eventsComplete=0;    //includes degraded ones
    uint64_t eventsDegraded=0;
    uint64_t eventsIncomplete=0;  //timed out
    uint64_t duplicates=0;
    uint64_t late=0;              //frag for an event thats already done
    uint64_t badSource=0;
    uint64_t maxPartials=0;
};

enum class AddResult { PENDING, COMPLETE, DUPLICATE, LATE, BAD_SOURCE };

class Assembler {
public:
    static constexpr int MAX_SOURCES=64;
    using EventFn=std::function<void(Event&&)>;

    Assembler(uint16_t numSources, uint64_t timeoutNs, uint32_t retiredWindow=1u<<16);

    //required = which sources must be there (normally all of them)
    //COMPLETE -> `out` holds the event. DUPLICATE/LATE/BAD_SOURCE -> caller frees the frag
    AddResult add(const Fragment& f, uint64_t now, uint64_t required, Event& out);

    //evict partials older than the timeout, they come out with complete=false
    void sweep(uint64_t now, const EventFn& out);

    //required got smaller (a source died, degraded mode) -> complete whatever is now satisfied
    void completeReady(uint64_t required, uint64_t now, const EventFn& out);

    //end of run, evict everything
    void flush(uint64_t now, const EventFn& out);

    const AssemblerStats& stats() const { return st; }
    const std::vector<uint64_t>& missingPerSource() const { return missing; }
    size_t partials() const { return pending.size(); }
    uint64_t fullMask() const { return full; }

private:
    bool isRetired(uint64_t id) const;
    void retire(uint64_t id);
    Event finish(std::unordered_map<uint64_t, Event>::iterator it, uint64_t now, bool done);

    uint16_t numSources;
    uint64_t full;
    uint64_t timeoutNs;

    std::unordered_map<uint64_t, Event> pending;
    //(firstArrival, eventId) in arrival order. completed events stay in here and get
    //skipped later -> sweep is O(expired) not O(all partials)
    std::deque<std::pair<uint64_t, uint64_t>> age;

    //recently finished ids, ring indexed by id % window. older than highest-window = late anyway
    std::vector<uint64_t> retired;
    uint64_t retiredMask;
    uint64_t highestSeen=0;

    std::vector<uint64_t> missing;  //per source: how often it was the missing one in a timeout
    AssemblerStats st;
};

} // namespace daq
