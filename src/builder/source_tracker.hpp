#pragma once
/*
 * SourceTracker: watches the seq / eventId stream of ONE source (runs in the receiver,
 * tcp keeps each source in order so this is the natural place).
 *
 *   seq jumps forward              -> frags lost in transit (provisional, might show up later)
 *   seq below expected, not seen   -> reordered, fills a hole from before
 *   seq below expected, seen       -> duplicate (dropped here, never reaches the assembler)
 *   eventId jumps more than seq    -> frontend dropped events before sending (DROP policy)
 *
 * keeps a bitmap of the last WINDOW seqs to tell dup from reorder.
 * resetConn() on reconnect resets the seq state but keeps the counters.
 */
#include <bitset>
#include <cstdint>

namespace daq {

class SourceTracker {
public:
    static constexpr uint32_t WINDOW=4096;
    enum class Verdict { NEW, REORDERED, DUPLICATE, TOO_OLD };

    void resetConn() {
        seen=false;
        nextSeq=0;
        nextEvent=0;
        bits.reset();
    }

    Verdict observe(uint32_t seq, uint64_t eventId) {
        frags++;
        if(!seen){
            //first frag on this conn (could be a restarted frontend joining mid run)
            seen=true;
            nextSeq=seq+1;
            nextEvent=eventId+1;
            mark(seq);
            return Verdict::NEW;
        }

        if(seq>=nextSeq){
            uint64_t gap=seq-nextSeq;
            seqGaps+=gap;
            //slots for the skipped seqs are "not seen" until they turn up
            uint64_t toClear=gap<WINDOW ? gap : WINDOW;
            for(uint64_t i=0;i<toClear;i++){
                bits.reset((nextSeq+i)%WINDOW);
            }
            mark(seq);
            if(eventId>=nextEvent){
                uint64_t jump=eventId-nextEvent;
                //only blame the source for what the seq gap doesnt explain
                sourceSkips+=jump>gap ? jump-gap : 0;
                nextEvent=eventId+1;
            }
            nextSeq=seq+1;
            return Verdict::NEW;
        }

        //seq went backwards
        bool tooOldSeq=nextSeq-seq>=WINDOW;
        if(tooOldSeq){
            tooOld++;
            return Verdict::TOO_OLD;
        }
        if(bits.test(seq%WINDOW)){
            duplicates++;
            return Verdict::DUPLICATE;
        }
        mark(seq);
        reordered++;
        return Verdict::REORDERED;
    }

    //holes that never got filled
    uint64_t netTransitLoss() const { return seqGaps>reordered ? seqGaps-reordered : 0; }

    uint64_t frags=0;
    uint64_t seqGaps=0;
    uint64_t reordered=0;
    uint64_t duplicates=0;
    uint64_t tooOld=0;
    uint64_t sourceSkips=0;

private:
    void mark(uint32_t seq) { bits.set(seq%WINDOW); }

    bool seen=false;
    uint32_t nextSeq=0;
    uint64_t nextEvent=0;
    std::bitset<WINDOW> bits;
};

} // namespace daq
