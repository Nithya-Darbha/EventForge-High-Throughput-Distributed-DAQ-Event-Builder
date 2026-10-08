#pragma once
/*
 * Time helpers. Everything is CLOCK_MONOTONIC ns.
 * On linux CLOCK_MONOTONIC is the same clock for every process on the box,
 * thats what makes trigger->sink latency across processes valid.
 */
#include <sys/prctl.h>

#include <cerrno>
#include <cstdint>
#include <ctime>

namespace daq {

inline constexpr uint64_t NS_PER_US=1'000ull;
inline constexpr uint64_t NS_PER_MS=1'000'000ull;
inline constexpr uint64_t NS_PER_S=1'000'000'000ull;

inline uint64_t nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return static_cast<uint64_t>(ts.tv_sec)*NS_PER_S+static_cast<uint64_t>(ts.tv_nsec);
}

inline void sleepUntilNs(uint64_t deadline) {
    timespec ts{static_cast<time_t>(deadline/NS_PER_S),static_cast<long>(deadline%NS_PER_S)};
    while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,nullptr)==EINTR){
    }
}

inline void sleepNs(uint64_t ns) {
    sleepUntilNs(nowNs()+ns);
}

//default timer slack is 50us -> every sleep overshoots by ~50us
//1ns slack makes short sleeps (trigger gen, sink delay) actually short
inline void tightTimerSlack() {
    prctl(PR_SET_TIMERSLACK,1UL,0,0,0);
}

inline double nsToUs(uint64_t ns) { return static_cast<double>(ns)/1e3; }
inline double nsToS(uint64_t ns) { return static_cast<double>(ns)/1e9; }

} // namespace daq
