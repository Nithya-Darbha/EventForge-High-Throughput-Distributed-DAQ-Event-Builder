#pragma once
/*
 * SpscRing: lock free ring for exactly ONE producer thread and ONE consumer thread.
 * Used receiver -> assembler shard (each shard has its own ring so its always 1:1).
 *
 * - head is only written by the consumer, tail only by the producer
 * - they sit on separate cache lines so the 2 threads dont fight over one line
 * - each side keeps a cached copy of the other index, only reloads it (= touches the
 *   other cache line) when the ring looks full/empty
 * - capacity rounded up to a power of 2 so idx & mask instead of %
 */
#include <atomic>
#include <bit>
#include <cstddef>
#include <vector>

namespace daq {

inline constexpr size_t CACHE_LINE=64;

template <class T>
class SpscRing {
public:
    explicit SpscRing(size_t cap) : buf(std::bit_ceil(cap)), mask(std::bit_ceil(cap)-1) {}

    //producer only
    bool tryPush(T&& v) {
        size_t t=tail.load(std::memory_order_relaxed);
        if(t-headCache>mask){
            //looks full, check the real head
            headCache=head.load(std::memory_order_acquire);
            if(t-headCache>mask) return false;
        }
        buf[t&mask]=std::move(v);
        tail.store(t+1,std::memory_order_release);  //publish the slot
        return true;
    }

    //consumer only
    bool tryPop(T& out) {
        size_t h=head.load(std::memory_order_relaxed);
        if(h==tailCache){
            //looks empty, check the real tail
            tailCache=tail.load(std::memory_order_acquire);
            if(h==tailCache) return false;
        }
        out=std::move(buf[h&mask]);
        head.store(h+1,std::memory_order_release);  //give the slot back
        return true;
    }

    //approx, any thread can call it
    size_t size() const {
        size_t t=tail.load(std::memory_order_relaxed);
        size_t h=head.load(std::memory_order_relaxed);
        return t>=h ? t-h : 0;
    }
    size_t capacity() const { return mask+1; }

private:
    std::vector<T> buf;
    const size_t mask;

    alignas(CACHE_LINE) std::atomic<size_t> head{0};  //consumer writes
    size_t tailCache=0;                                //consumer's copy of tail
    alignas(CACHE_LINE) std::atomic<size_t> tail{0};  //producer writes
    size_t headCache=0;                                //producer's copy of head
};

} // namespace daq
