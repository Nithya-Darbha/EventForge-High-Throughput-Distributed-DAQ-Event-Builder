#pragma once
/*
 * BufferPool: one big slab cut into fixed size slots, allocated once at startup.
 * The receiver copies each fragment payload into a slot and only the slot index
 * moves thru the queues. Whoever is done with the frag (sink, or the shard when it
 * drops a dup/late frag or evicts an incomplete event) gives the slot back.
 * -> no malloc for payload bytes on the hot path
 * -> pool size = hard cap on memory, running out = 2nd backpressure point
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace daq {

class BufferPool {
public:
    static constexpr uint32_t NONE=UINT32_MAX;

    BufferPool(uint32_t slots, uint32_t bytesPerSlot)
        : slab(static_cast<size_t>(slots)*bytesPerSlot), slotSize(bytesPerSlot), total(slots) {
        if(slots==0 || bytesPerSlot==0) throw std::invalid_argument("empty pool");
        freeList.reserve(slots);
        for(uint32_t i=slots;i>0;i--){
            freeList.push_back(i-1);  //so slot 0 is handed out first
        }
    }

    //NONE if the pool is empty
    uint32_t acquire() {
        std::lock_guard<std::mutex> lk(mtx);
        if(freeList.empty()) return NONE;
        uint32_t idx=freeList.back();
        freeList.pop_back();
        used.fetch_add(1,std::memory_order_relaxed);
        return idx;
    }

    void release(uint32_t idx) {
        std::lock_guard<std::mutex> lk(mtx);
        freeList.push_back(idx);
        used.fetch_sub(1,std::memory_order_relaxed);
    }

    std::byte* data(uint32_t idx) { return slab.data()+static_cast<size_t>(idx)*slotSize; }
    const std::byte* data(uint32_t idx) const { return slab.data()+static_cast<size_t>(idx)*slotSize; }

    uint32_t inUse() const { return used.load(std::memory_order_relaxed); }
    uint32_t capacity() const { return total; }
    uint32_t slotBytes() const { return slotSize; }

private:
    std::vector<std::byte> slab;
    uint32_t slotSize;
    uint32_t total;
    std::mutex mtx;
    std::vector<uint32_t> freeList;
    std::atomic<uint32_t> used{0};
};

} // namespace daq
