#include <gtest/gtest.h>

#include "builder/source_tracker.hpp"

using namespace daq;
using V=SourceTracker::Verdict;

TEST(SourceTracker, CleanStream) {
    SourceTracker t;
    for(uint32_t i=0;i<100;i++) EXPECT_EQ(t.observe(i,i),V::NEW);
    EXPECT_EQ(t.seqGaps,0u);
    EXPECT_EQ(t.sourceSkips,0u);
    EXPECT_EQ(t.netTransitLoss(),0u);
}

//seq jumped -> frags vanished after the frontend gave them a seq
TEST(SourceTracker, SeqGapIsTransitLoss) {
    SourceTracker t;
    t.observe(0,0);
    t.observe(3,3);
    EXPECT_EQ(t.seqGaps,2u);
    EXPECT_EQ(t.netTransitLoss(),2u);
    EXPECT_EQ(t.sourceSkips,0u);
}

//eventId jumped but seq didnt -> frontend dropped them before sending (DROP policy)
TEST(SourceTracker, EventJumpIsSourceDrop) {
    SourceTracker t;
    t.observe(0,0);
    t.observe(1,5);
    EXPECT_EQ(t.sourceSkips,4u);
    EXPECT_EQ(t.seqGaps,0u);
}

TEST(SourceTracker, MixedDropAndLoss) {
    SourceTracker t;
    t.observe(0,0);
    t.observe(2,6);  //5 events missing: 1 had a seq (lost), 4 never got one (dropped)
    EXPECT_EQ(t.seqGaps,1u);
    EXPECT_EQ(t.sourceSkips,4u);
}

TEST(SourceTracker, ReorderFillsTheHole) {
    SourceTracker t;
    t.observe(0,0);
    EXPECT_EQ(t.observe(2,2),V::NEW);
    EXPECT_EQ(t.observe(1,1),V::REORDERED);
    EXPECT_EQ(t.seqGaps,1u);
    EXPECT_EQ(t.reordered,1u);
    EXPECT_EQ(t.netTransitLoss(),0u);
}

TEST(SourceTracker, DuplicateNotMistakenForReorder) {
    SourceTracker t;
    t.observe(0,0);
    t.observe(1,1);
    EXPECT_EQ(t.observe(1,1),V::DUPLICATE);
    EXPECT_EQ(t.duplicates,1u);
    EXPECT_EQ(t.reordered,0u);
    //and a dup of a frag that came in reordered
    t.observe(3,3);
    EXPECT_EQ(t.observe(2,2),V::REORDERED);
    EXPECT_EQ(t.observe(2,2),V::DUPLICATE);
    EXPECT_EQ(t.netTransitLoss(),0u);
}

TEST(SourceTracker, WayTooOld) {
    SourceTracker t;
    t.observe(0,0);
    t.observe(SourceTracker::WINDOW+10,SourceTracker::WINDOW+10);
    EXPECT_EQ(t.observe(1,1),V::TOO_OLD);
}

//restarted frontend starts at seq 0 again, mid run eventId. must not look like a reorder/dup
TEST(SourceTracker, ResetOnReconnectKeepsCounters) {
    SourceTracker t;
    t.observe(0,0);
    t.observe(5,5);
    EXPECT_EQ(t.seqGaps,4u);
    t.resetConn();
    EXPECT_EQ(t.observe(0,9000),V::NEW);
    EXPECT_EQ(t.observe(1,9001),V::NEW);
    EXPECT_EQ(t.seqGaps,4u);
    EXPECT_EQ(t.sourceSkips,0u);
}
