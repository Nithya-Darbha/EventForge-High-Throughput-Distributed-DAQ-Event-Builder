#include <gtest/gtest.h>

#include <vector>

#include "builder/assembler.hpp"

using namespace daq;

namespace {

Fragment frag(uint16_t src, uint64_t evt, uint32_t len=16) {
    Fragment f;
    f.hdr.sourceId=src;
    f.hdr.eventId=evt;
    f.hdr.payloadLen=len;
    f.hdr.triggerTs=evt*1000;
    f.buf=static_cast<uint32_t>(evt*64+src);
    return f;
}

constexpr uint64_t TIMEOUT=1'000'000;  //1 ms
constexpr uint64_t ALL3=0b111;

struct Collect {
    std::vector<Event> evs;
    Assembler::EventFn fn=[this](Event&& e){ evs.push_back(std::move(e)); };
};

} // namespace

TEST(Assembler, CompletesWhenAllSourcesArrive) {
    Assembler a(3,TIMEOUT);
    Event ev;
    EXPECT_EQ(a.add(frag(0,5),100,ALL3,ev),AddResult::PENDING);
    EXPECT_EQ(a.add(frag(2,5),110,ALL3,ev),AddResult::PENDING);
    ASSERT_EQ(a.add(frag(1,5),120,ALL3,ev),AddResult::COMPLETE);
    EXPECT_TRUE(ev.complete);
    EXPECT_FALSE(ev.degraded);
    EXPECT_EQ(ev.eventId,5u);
    EXPECT_EQ(ev.frags.size(),3u);
    EXPECT_EQ(ev.bytes,48u);
    EXPECT_EQ(ev.firstArrival,100u);
    EXPECT_EQ(ev.doneTs,120u);
    EXPECT_EQ(a.partials(),0u);
    EXPECT_EQ(a.stats().eventsComplete,1u);
}

//source 1 is 2 events ahead of source 0 -> 2 partials open at once, both finish later
TEST(Assembler, InterleavedEvents) {
    Assembler a(2,TIMEOUT);
    Event ev;
    a.add(frag(1,0),1,0b11,ev);
    a.add(frag(1,1),2,0b11,ev);
    EXPECT_EQ(a.add(frag(0,0),3,0b11,ev),AddResult::COMPLETE);
    EXPECT_EQ(a.add(frag(0,1),4,0b11,ev),AddResult::COMPLETE);
    EXPECT_EQ(a.stats().maxPartials,2u);
}

TEST(Assembler, DuplicateRejected) {
    Assembler a(2,TIMEOUT);
    Event ev;
    a.add(frag(0,1),1,0b11,ev);
    EXPECT_EQ(a.add(frag(0,1),2,0b11,ev),AddResult::DUPLICATE);
    EXPECT_EQ(a.stats().duplicates,1u);
    ASSERT_EQ(a.add(frag(1,1),3,0b11,ev),AddResult::COMPLETE);
    EXPECT_EQ(ev.frags.size(),2u);  //dup didnt sneak in
}

TEST(Assembler, TimeoutEvictsAndBlamesMissingSource) {
    Assembler a(3,TIMEOUT);
    Event ev;
    a.add(frag(0,7),1000,ALL3,ev);
    a.add(frag(1,7),1000,ALL3,ev);
    Collect c;
    a.sweep(1000+TIMEOUT-1,c.fn);
    EXPECT_TRUE(c.evs.empty());
    a.sweep(1000+TIMEOUT,c.fn);
    ASSERT_EQ(c.evs.size(),1u);
    EXPECT_FALSE(c.evs[0].complete);
    EXPECT_EQ(c.evs[0].presentMask,0b011u);
    EXPECT_EQ(c.evs[0].frags.size(),2u);  //caller needs these to free the buffers
    EXPECT_EQ(a.stats().eventsIncomplete,1u);
    EXPECT_EQ(a.missingPerSource()[2],1u);
    EXPECT_EQ(a.missingPerSource()[0],0u);
}

//after an event got evicted, a straggler must not open a new zombie partial
TEST(Assembler, LateFragAfterEviction) {
    Assembler a(2,TIMEOUT);
    Event ev;
    a.add(frag(0,3),0,0b11,ev);
    Collect c;
    a.sweep(TIMEOUT,c.fn);
    ASSERT_EQ(c.evs.size(),1u);
    EXPECT_EQ(a.add(frag(1,3),TIMEOUT+1,0b11,ev),AddResult::LATE);
    EXPECT_EQ(a.stats().late,1u);
    EXPECT_EQ(a.partials(),0u);
}

TEST(Assembler, LateFragAfterCompletion) {
    Assembler a(2,TIMEOUT);
    Event ev;
    a.add(frag(0,3),0,0b11,ev);
    ASSERT_EQ(a.add(frag(1,3),1,0b11,ev),AddResult::COMPLETE);
    EXPECT_EQ(a.add(frag(1,3),2,0b11,ev),AddResult::LATE);
    EXPECT_EQ(a.partials(),0u);
}

TEST(Assembler, CompletedEventsDontTimeOutLater) {
    Assembler a(1,TIMEOUT);
    Event ev;
    ASSERT_EQ(a.add(frag(0,0),0,0b1,ev),AddResult::COMPLETE);
    Collect c;
    a.sweep(10*TIMEOUT,c.fn);
    EXPECT_TRUE(c.evs.empty());
    EXPECT_EQ(a.stats().eventsIncomplete,0u);
}

TEST(Assembler, BadSourceRejected) {
    Assembler a(2,TIMEOUT);
    Event ev;
    EXPECT_EQ(a.add(frag(5,0),0,0b11,ev),AddResult::BAD_SOURCE);
    EXPECT_EQ(a.partials(),0u);
}

TEST(Assembler, VeryOldEventIsLate) {
    Assembler a(2,TIMEOUT,16);
    Event ev;
    a.add(frag(0,100),0,0b11,ev);
    EXPECT_EQ(a.add(frag(0,50),1,0b11,ev),AddResult::LATE);  //50+16 <= 100, never tracked
}

//degraded mode: source 2 dead -> required is just 0 and 1
TEST(Assembler, DegradedCompletionWithSmallerMask) {
    Assembler a(3,TIMEOUT);
    Event ev;
    a.add(frag(0,1),0,0b011,ev);
    ASSERT_EQ(a.add(frag(1,1),1,0b011,ev),AddResult::COMPLETE);
    EXPECT_TRUE(ev.degraded);
    EXPECT_EQ(a.stats().eventsDegraded,1u);
}

//source dies while events wait for it -> completeReady finishes the ones that are now ok
TEST(Assembler, CompleteReadyAfterSourceDies) {
    Assembler a(3,TIMEOUT);
    Event ev;
    a.add(frag(0,1),0,ALL3,ev);
    a.add(frag(1,1),0,ALL3,ev);  //waiting for 2
    a.add(frag(0,2),0,ALL3,ev);  //waiting for 1 and 2
    Collect c;
    a.completeReady(0b011,5,c.fn);
    ASSERT_EQ(c.evs.size(),1u);
    EXPECT_EQ(c.evs[0].eventId,1u);
    EXPECT_TRUE(c.evs[0].degraded);
    EXPECT_EQ(a.partials(),1u);
}

TEST(Assembler, FlushEvictsEverything) {
    Assembler a(2,TIMEOUT);
    Event ev;
    for(uint64_t e=0;e<10;e++) a.add(frag(0,e),e,0b11,ev);
    Collect c;
    a.flush(20,c.fn);
    EXPECT_EQ(c.evs.size(),10u);
    EXPECT_EQ(a.partials(),0u);
    EXPECT_EQ(a.missingPerSource()[1],10u);
}

TEST(Assembler, SixtyFourSources) {
    Assembler a(64,TIMEOUT);
    Event ev;
    for(uint16_t s=0;s<63;s++) EXPECT_EQ(a.add(frag(s,0),0,~0ull,ev),AddResult::PENDING);
    ASSERT_EQ(a.add(frag(63,0),0,~0ull,ev),AddResult::COMPLETE);
    EXPECT_EQ(ev.presentMask,~0ull);
}
