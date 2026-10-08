#pragma once
/*
 * Small observability helpers:
 * - Counter: relaxed atomic padded to its own cache line (no false sharing between threads)
 * - JsonLine: builds one json object as a string, for the metrics.jsonl lines + summary
 * - cpu helpers: process cpu time, and per thread cpu from /proc/self/task
 */
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#include "daq/spsc_ring.hpp"

namespace daq {

struct alignas(CACHE_LINE) Counter {
    std::atomic<uint64_t> v{0};
    void add(uint64_t n=1) { v.fetch_add(n,std::memory_order_relaxed); }
    void set(uint64_t n) { v.store(n,std::memory_order_relaxed); }
    uint64_t get() const { return v.load(std::memory_order_relaxed); }
};

class JsonLine {
public:
    JsonLine& add(const std::string& k, double v);
    JsonLine& add(const std::string& k, uint64_t v);
    JsonLine& add(const std::string& k, int v) { return add(k,static_cast<uint64_t>(v)); }
    JsonLine& add(const std::string& k, const std::string& v);  //quoted
    JsonLine& raw(const std::string& k, const std::string& json);  //already json (object/array)
    std::string str() const { return "{"+body+"}"; }

private:
    void key(const std::string& k);
    std::string body;
};

double processCpuSeconds();

//thread name -> cpu seconds, threads named with pthread_setname_np
std::map<std::string, double> threadCpuSeconds();

void nameThread(const std::string& name);

} // namespace daq
