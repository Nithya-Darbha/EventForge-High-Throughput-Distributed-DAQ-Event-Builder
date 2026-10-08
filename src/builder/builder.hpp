#pragma once
/*
 * Stuff shared by all the builder threads:
 *   BuilderConfig  - every knob, from the cmd line
 *   Shared         - buffer pool, per source credit counters, live source mask
 *   ShardInbox     - receiver -> shard queue (lock free SPSC ring or mutex queue, cmd line switch)
 */
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "builder/assembler.hpp"
#include "daq/args.hpp"
#include "daq/bounded_queue.hpp"
#include "daq/buffer_pool.hpp"
#include "daq/spsc_ring.hpp"

namespace daq {

struct BuilderConfig {
    uint16_t port=9000;
    uint16_t sources=4;
    uint32_t credits=256;        //max frags in flight per source (builder side buffering)
    uint32_t shards=2;           //assembler threads
    uint32_t sinks=2;            //sink worker threads
    bool spscQueues=true;        //receiver->shard: lock free ring or mutex queue
    uint32_t inboxCap=0;         //0 = big enough to never fill (sources*credits)
    uint32_t doneCap=1024;       //shards->sinks queue, in events
    uint64_t timeoutNs=100'000'000;
    bool degraded=false;         //complete events w/o dead sources
    uint32_t maxPayload=8192;
    //trigger
    double rate=10000;
    std::string rateProfile;     //"t:rate,t:rate,..." overrides rate
    double durationS=5;
    uint64_t ttcBatchNs=20'000;
    uint64_t startDelayNs=200'000'000;
    //sink
    uint64_t sinkDelayNs=0;      //simulated storage time per event
    double slowAtS=-1;           //consumer slowdown fault
    double slowForS=0;
    uint64_t slowDelayNs=0;
    bool verify=false;
    //output
    std::string metricsOut;
    uint64_t sampleNs=100'000'000;
    double maxRunS=120;
    bool quiet=false;
    bool debug=false;

    static BuilderConfig fromArgs(const Args& a);
    uint32_t poolSlots() const { return sources*(credits+8u); }  //+8: a reorder/dup can overshoot credit by a frag or 2
};

struct alignas(64) SourceCredit {
    std::atomic<uint64_t> released{0};  //frags of this source we are done with (this conn)
    std::atomic<uint32_t> gen{0};       //++ on every new conn, old frags dont count for the new one
    std::atomic<uint64_t> wakeAt{UINT64_MAX};  //released count at which the receiver wants a poke
};

class Shared {
public:
    explicit Shared(const BuilderConfig& cfg)
        : pool(cfg.poolSlots(),cfg.maxPayload), credit(new SourceCredit[cfg.sources]),
          wakeFd(eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC)) {}
    ~Shared() { close(wakeFd); }
    Shared(const Shared&)=delete;
    Shared& operator=(const Shared&)=delete;

    //done with a frag: payload slot back to the pool + 1 credit back to its source
    void releaseFrag(const Fragment& f) {
        pool.release(f.buf);
        releaseCredit(f.hdr.sourceId,f.gen);
    }
    //frag that never got a pool slot (dup/crc error), just the credit
    //exactly when enough came back for the next CREDIT msg, poke the receiver thru the
    //eventfd. before this, if every frontend was blocked (= no socket traffic) the receiver
    //only noticed on its 1ms epoll timeout -> credits came back late (worse in a VM)
    void releaseCredit(uint16_t src, uint32_t gen) {
        SourceCredit& c=credit[src];
        if(c.gen.load(std::memory_order_acquire)!=gen) return;
        uint64_t r=c.released.fetch_add(1,std::memory_order_seq_cst)+1;
        if(r==c.wakeAt.load(std::memory_order_seq_cst)){
            uint64_t one=1;
            ssize_t n=write(wakeFd,&one,sizeof(one));
            (void)n;  //EAGAIN = counter already set, receiver wakes anyway
        }
    }

    BufferPool pool;
    std::unique_ptr<SourceCredit[]> credit;
    std::atomic<uint64_t> liveMask{0};  //sources currently connected
    int wakeFd;                         //eventfd, receiver has it in its epoll set
};

class ShardInbox {
public:
    ShardInbox(bool spsc, size_t cap);

    bool tryPush(Fragment&& f);
    //false on timeout, or closed and empty
    bool popFor(Fragment& f, uint64_t timeoutNs);
    bool tryPop(Fragment& f);
    void close();
    bool closedAndEmpty() const;
    size_t size() const;
    size_t capacity() const;

private:
    std::unique_ptr<SpscRing<Fragment>> ring;
    std::unique_ptr<BoundedQueue<Fragment>> q;
    std::atomic<bool> closed{false};
    //spsc ring has no condvar: consumer sleeps on a futex, producer bumps pushSeq and
    //wakes it only if `sleeping` is set (so the fast path is just an increment + a load)
    alignas(64) std::atomic<uint32_t> pushSeq{0};
    alignas(64) std::atomic<uint32_t> sleeping{0};
};

} // namespace daq
