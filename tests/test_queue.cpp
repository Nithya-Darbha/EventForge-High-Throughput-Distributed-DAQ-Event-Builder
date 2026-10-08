#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "daq/bounded_queue.hpp"
#include "daq/buffer_pool.hpp"
#include "daq/spsc_ring.hpp"

using namespace daq;

//---------- BoundedQueue ----------

TEST(BoundedQueue, FifoAndCapacity) {
    BoundedQueue<int> q(3);
    EXPECT_TRUE(q.tryPush(1));
    EXPECT_TRUE(q.tryPush(2));
    EXPECT_TRUE(q.tryPush(3));
    EXPECT_FALSE(q.tryPush(4));  //full
    EXPECT_EQ(q.size(),3u);
    int v=0;
    EXPECT_TRUE(q.tryPop(v));
    EXPECT_EQ(v,1);
    EXPECT_TRUE(q.tryPush(4));   //wraps around
    for(int want:{2,3,4}){
        EXPECT_TRUE(q.tryPop(v));
        EXPECT_EQ(v,want);
    }
    EXPECT_FALSE(q.tryPop(v));
}

//push on a full queue has to block until a consumer makes room = backpressure
TEST(BoundedQueue, PushBlocksWhileFull) {
    BoundedQueue<int> q(1);
    q.push(1);
    std::atomic<bool> pushed{false};
    std::thread t([&]{
        q.push(2);
        pushed=true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(pushed.load());
    int v=0;
    q.pop(v);
    t.join();
    EXPECT_TRUE(pushed.load());
    q.pop(v);
    EXPECT_EQ(v,2);
}

TEST(BoundedQueue, CloseDrainsThenFails) {
    BoundedQueue<int> q(4);
    q.push(1);
    q.push(2);
    q.close();
    EXPECT_FALSE(q.push(3));
    int v=0;
    EXPECT_TRUE(q.pop(v));
    EXPECT_TRUE(q.pop(v));
    EXPECT_FALSE(q.pop(v));  //closed + empty
}

TEST(BoundedQueue, CloseWakesBlockedConsumer) {
    BoundedQueue<int> q(4);
    std::thread t([&]{
        int v=0;
        EXPECT_FALSE(q.pop(v));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.close();
    t.join();
}

TEST(BoundedQueue, PopForTimesOut) {
    BoundedQueue<int> q(4);
    int v=0;
    EXPECT_FALSE(q.popFor(v,5'000'000));
}

//4 producers 4 consumers, every item comes out exactly once
TEST(BoundedQueue, MpmcStress) {
    BoundedQueue<uint64_t> q(64);
    const uint64_t perProducer=50'000;
    std::atomic<uint64_t> sum{0};
    std::atomic<uint64_t> count{0};
    std::vector<std::thread> th;
    for(uint64_t p=0;p<4;p++){
        th.emplace_back([&,p]{
            for(uint64_t i=0;i<perProducer;i++) q.push(p*perProducer+i+1);
        });
    }
    std::vector<std::thread> cons;
    for(int c=0;c<4;c++){
        cons.emplace_back([&]{
            uint64_t v=0;
            while(q.pop(v)){
                sum+=v;
                count++;
            }
        });
    }
    for(auto& t:th) t.join();
    q.close();
    for(auto& t:cons) t.join();
    uint64_t n=4*perProducer;
    EXPECT_EQ(count.load(),n);
    EXPECT_EQ(sum.load(),n*(n+1)/2);
}

//---------- SpscRing ----------

TEST(SpscRing, RoundsCapacityUpToPow2) {
    SpscRing<int> r(5);
    EXPECT_EQ(r.capacity(),8u);
}

TEST(SpscRing, FullAndEmpty) {
    SpscRing<int> r(4);
    int v=0;
    EXPECT_FALSE(r.tryPop(v));
    for(int i=0;i<4;i++) EXPECT_TRUE(r.tryPush(int(i)));
    EXPECT_FALSE(r.tryPush(99));
    EXPECT_EQ(r.size(),4u);
    for(int i=0;i<4;i++){
        EXPECT_TRUE(r.tryPop(v));
        EXPECT_EQ(v,i);
    }
    EXPECT_FALSE(r.tryPop(v));
}

//real threads, items must come out in order with nothing missing (tsan build checks the atomics)
TEST(SpscRing, TwoThreadOrdering) {
    SpscRing<uint64_t> r(256);
    const uint64_t n=2'000'000;
    std::thread prod([&]{
        for(uint64_t i=0;i<n;i++){
            while(!r.tryPush(uint64_t(i))){
            }
        }
    });
    uint64_t expect=0;
    uint64_t v=0;
    while(expect<n){
        if(r.tryPop(v)){
            ASSERT_EQ(v,expect);
            expect++;
        }
    }
    prod.join();
}

//---------- BufferPool ----------

TEST(BufferPool, AcquireReleaseExhaust) {
    BufferPool p(4,128);
    std::set<uint32_t> got;
    for(int i=0;i<4;i++){
        uint32_t idx=p.acquire();
        ASSERT_NE(idx,BufferPool::NONE);
        got.insert(idx);
    }
    EXPECT_EQ(got.size(),4u);  //all different slots
    EXPECT_EQ(p.acquire(),BufferPool::NONE);
    EXPECT_EQ(p.inUse(),4u);
    p.release(*got.begin());
    EXPECT_EQ(p.inUse(),3u);
    EXPECT_NE(p.acquire(),BufferPool::NONE);
}

TEST(BufferPool, SlotsDontOverlap) {
    BufferPool p(3,64);
    uint32_t a=p.acquire();
    uint32_t b=p.acquire();
    EXPECT_EQ(p.data(b)-p.data(a),64*(static_cast<long>(b)-static_cast<long>(a)));
}
