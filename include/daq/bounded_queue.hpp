#pragma once
/*
 * BoundedQueue: multi producer / multi consumer queue with a fixed capacity.
 * mutex + 2 condvars, nothing clever. push() blocks while full -> thats the
 * backpressure, a slow consumer stalls whoever pushes into it.
 * close() wakes everyone up, pop() then drains whats left and returns false.
 */
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <vector>

namespace daq {

template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : buf(capacity), cap(capacity) {}

    //blocks while full, false if the queue got closed
    bool push(T&& v) {
        std::unique_lock<std::mutex> lk(mtx);
        notFull.wait(lk,[&]{ return count<cap || closed; });
        if(closed) return false;
        pushLocked(std::move(v));
        lk.unlock();
        notEmpty.notify_one();
        return true;
    }

    bool tryPush(T&& v) {
        std::unique_lock<std::mutex> lk(mtx);
        if(count==cap || closed) return false;
        pushLocked(std::move(v));
        lk.unlock();
        notEmpty.notify_one();
        return true;
    }

    //blocks while empty, false only when closed AND empty
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mtx);
        notEmpty.wait(lk,[&]{ return count>0 || closed; });
        if(count==0) return false;
        popLocked(out);
        lk.unlock();
        notFull.notify_one();
        return true;
    }

    //same as pop but gives up after timeoutNs
    bool popFor(T& out, uint64_t timeoutNs) {
        std::unique_lock<std::mutex> lk(mtx);
        bool ready=notEmpty.wait_for(lk,std::chrono::nanoseconds(timeoutNs),[&]{ return count>0 || closed; });
        if(!ready || count==0) return false;
        popLocked(out);
        lk.unlock();
        notFull.notify_one();
        return true;
    }

    bool tryPop(T& out) {
        std::unique_lock<std::mutex> lk(mtx);
        if(count==0) return false;
        popLocked(out);
        lk.unlock();
        notFull.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            closed=true;
        }
        notEmpty.notify_all();
        notFull.notify_all();
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lk(mtx);
        return closed;
    }

    //no lock, for the metrics sampler. can be slightly stale which is fine
    size_t size() const { return depth.load(std::memory_order_relaxed); }
    size_t capacity() const { return cap; }

private:
    void pushLocked(T&& v) {
        buf[tail]=std::move(v);
        tail=(tail+1)%cap;
        count++;
        depth.store(count,std::memory_order_relaxed);
    }
    void popLocked(T& out) {
        out=std::move(buf[head]);
        head=(head+1)%cap;
        count--;
        depth.store(count,std::memory_order_relaxed);
    }

    mutable std::mutex mtx;
    std::condition_variable notEmpty;
    std::condition_variable notFull;
    std::vector<T> buf;
    size_t cap;
    size_t head=0;
    size_t tail=0;
    size_t count=0;
    bool closed=false;
    std::atomic<size_t> depth{0};
};

} // namespace daq
