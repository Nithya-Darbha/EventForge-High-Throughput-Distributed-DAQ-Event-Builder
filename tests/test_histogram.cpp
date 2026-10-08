#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "daq/histogram.hpp"

using namespace daq;

//every value has to land in a bucket thats >= the previous one and <= its upper edge
TEST(Histogram, BucketsMonotonicAndContiguous) {
    int prev=-1;
    for(uint64_t v=0;v<1'000'000;v++){
        int b=Histogram::bucketOf(v);
        ASSERT_TRUE(b==prev || b==prev+1) << "v=" << v;
        ASSERT_LE(v,Histogram::bucketUpper(b));
        prev=b;
    }
    EXPECT_LT(Histogram::bucketOf(UINT64_MAX),Histogram::BUCKETS);
}

TEST(Histogram, SmallValuesExact) {
    Histogram h;
    for(uint64_t v=1;v<=50;v++) h.record(v);
    EXPECT_EQ(h.percentile(50),25u);
    EXPECT_EQ(h.percentile(100),50u);
    EXPECT_EQ(h.min(),1u);
}

//compare against exact percentiles from a sorted copy, should be within ~3%
TEST(Histogram, PercentilesWithinRelativeError) {
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> d(11.0,1.0);  //median ~60us in ns, long tail like real latency
    std::vector<uint64_t> v;
    Histogram h;
    for(int i=0;i<200'000;i++){
        auto x=static_cast<uint64_t>(d(rng));
        v.push_back(x);
        h.record(x);
    }
    std::sort(v.begin(),v.end());
    for(double p:{50.0,95.0,99.0,99.9}){
        auto exact=static_cast<double>(v[static_cast<size_t>(p/100.0*static_cast<double>(v.size()))-1]);
        auto got=static_cast<double>(h.percentile(p));
        EXPECT_NEAR(got/exact,1.0,0.035) << "p" << p;
    }
}

TEST(Histogram, Merge) {
    Histogram a;
    Histogram b;
    for(int i=0;i<100;i++) a.record(10);
    for(int i=0;i<100;i++) b.record(1000);
    a.merge(b);
    EXPECT_EQ(a.count(),200u);
    EXPECT_EQ(a.min(),10u);
    EXPECT_GE(a.max(),1000u);
    EXPECT_EQ(a.percentile(50),10u);
}
