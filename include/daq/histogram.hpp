#pragma once
/*
 * Log-linear latency histogram (HdrHistogram idea, much simpler).
 * 0..63 are exact buckets, above that every power of 2 gets 32 sub buckets
 * -> worst case relative error ~3%. Fixed size (1920 counters), O(1) record,
 * and two histograms can be merged (one per thread, merge at the end).
 */
#include <array>
#include <cstdint>

namespace daq {

class Histogram {
public:
    static constexpr int SUB_BITS=5;
    static constexpr int SUB=1<<SUB_BITS;                     //32
    static constexpr int LINEAR=2*SUB;                        //64 exact buckets
    static constexpr int BUCKETS=(64-SUB_BITS)*SUB+SUB;      //enough for any uint64

    static int bucketOf(uint64_t v);
    static uint64_t bucketUpper(int idx);  //biggest value that lands in idx

    void record(uint64_t v);
    void merge(const Histogram& o);
    void reset();

    uint64_t count() const { return cnt; }
    uint64_t min() const { return cnt ? minV : 0; }
    uint64_t max() const { return maxV; }
    double mean() const { return cnt ? static_cast<double>(sum)/static_cast<double>(cnt) : 0.0; }
    uint64_t percentile(double p) const;  //p in [0,100], returns bucket upper edge (conservative)

private:
    std::array<uint64_t, BUCKETS> b{};
    uint64_t cnt=0;
    uint64_t sum=0;
    uint64_t minV=UINT64_MAX;
    uint64_t maxV=0;
};

} // namespace daq
