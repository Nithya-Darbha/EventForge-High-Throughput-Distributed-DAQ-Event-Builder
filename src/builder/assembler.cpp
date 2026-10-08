#include "builder/assembler.hpp"

#include <bit>
#include <stdexcept>

namespace daq {

Assembler::Assembler(uint16_t n, uint64_t timeout, uint32_t retiredWindow)
    : numSources(n),
      full(n==64 ? ~0ull : ((1ull<<n)-1)),
      timeoutNs(timeout),
      retired(std::bit_ceil(retiredWindow),UINT64_MAX),
      retiredMask(std::bit_ceil(retiredWindow)-1),
      missing(n,0) {
    if(n==0 || n>MAX_SOURCES) throw std::invalid_argument("numSources must be 1..64");
    pending.reserve(4096);
}

bool Assembler::isRetired(uint64_t id) const {
    bool tooOld=highestSeen>retiredMask && id<=highestSeen-retiredMask-1;
    if(tooOld) return true;
    return retired[id&retiredMask]==id;
}

void Assembler::retire(uint64_t id) {
    retired[id&retiredMask]=id;
}

/*
event not pending
  -> already done before? -> LATE
  -> new partial
bit for this source already set -> DUPLICATE
set bit, keep frag
all required there -> COMPLETE (degraded if smtg is missing vs the full mask)
*/
AddResult Assembler::add(const Fragment& f, uint64_t now, uint64_t required, Event& out) {
    const auto& h=f.hdr;
    st.fragmentsIn++;
    if(h.sourceId>=numSources){
        st.badSource++;
        return AddResult::BAD_SOURCE;
    }

    auto it=pending.find(h.eventId);
    if(it==pending.end()){
        if(isRetired(h.eventId)){
            st.late++;
            return AddResult::LATE;
        }
        it=pending.try_emplace(h.eventId).first;
        if(h.eventId>highestSeen) highestSeen=h.eventId;
        Event& fresh=it->second;
        fresh.eventId=h.eventId;
        fresh.triggerTs=h.triggerTs;
        fresh.firstArrival=now;
        fresh.frags.reserve(numSources);
        age.emplace_back(now,h.eventId);
        if(pending.size()>st.maxPartials) st.maxPartials=pending.size();
    }
    Event& ev=it->second;

    const uint64_t bit=1ull<<h.sourceId;
    if(ev.presentMask&bit){
        st.duplicates++;
        return AddResult::DUPLICATE;
    }
    ev.presentMask|=bit;
    ev.bytes+=h.payloadLen;
    ev.frags.push_back(f);

    bool allThere=required!=0 && (ev.presentMask&required)==required;
    if(!allThere) return AddResult::PENDING;

    out=finish(it,now,true);
    return AddResult::COMPLETE;
} /* Assembler::add() */

//take the event out of pending, mark it retired, update stats
Event Assembler::finish(std::unordered_map<uint64_t, Event>::iterator it, uint64_t now, bool done) {
    Event ev=std::move(it->second);
    pending.erase(it);
    retire(ev.eventId);
    ev.doneTs=now;
    ev.complete=done;
    if(done){
        ev.degraded=ev.presentMask!=full;
        st.eventsComplete++;
        if(ev.degraded) st.eventsDegraded++;
    }
    else{
        st.eventsIncomplete++;
        uint64_t miss=full&~ev.presentMask;
        while(miss){
            int s=std::countr_zero(miss);
            missing[static_cast<size_t>(s)]++;
            miss&=miss-1;
        }
    }
    return ev;
}

/*
pop from the front of age while older than timeout
still pending -> evict as incomplete
not pending (completed already) -> just skip
*/
void Assembler::sweep(uint64_t now, const EventFn& out) {
    while(!age.empty() && now-age.front().first>=timeoutNs){
        uint64_t id=age.front().second;
        age.pop_front();
        auto it=pending.find(id);
        if(it==pending.end()) continue;
        out(finish(it,now,false));
    }
}

void Assembler::completeReady(uint64_t required, uint64_t now, const EventFn& out) {
    if(required==0) return;
    for(auto it=pending.begin();it!=pending.end();){
        bool ready=(it->second.presentMask&required)==required;
        if(ready){
            auto doneIt=it++;
            out(finish(doneIt,now,true));
        }
        else{
            ++it;
        }
    }
}

void Assembler::flush(uint64_t now, const EventFn& out) {
    while(!age.empty()){
        uint64_t id=age.front().second;
        age.pop_front();
        auto it=pending.find(id);
        if(it!=pending.end()) out(finish(it,now,false));
    }
}

} // namespace daq
