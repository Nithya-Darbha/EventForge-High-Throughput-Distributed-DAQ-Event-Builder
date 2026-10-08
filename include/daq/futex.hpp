#pragma once
/*
 * Raw linux futex on a std::atomic<uint32_t>.
 * wait: sleep while *addr==expected (or timeout). wake: wake up to n sleepers.
 * shared=true for futexes in shared memory between processes (TTC),
 * false (PRIVATE, a bit cheaper) for threads in one process.
 * This is what condvars use underneath, but here the wakeup is an event, not a timer,
 * which matters in VMs where short timers are slow.
 */
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#include <cstdint>
#include <ctime>

namespace daq {

static_assert(sizeof(std::atomic<uint32_t>)==4);

inline long futexWait(std::atomic<uint32_t>* addr, uint32_t expected, uint64_t timeoutNs, bool shared=false) {
    timespec ts{static_cast<time_t>(timeoutNs/1'000'000'000ull),static_cast<long>(timeoutNs%1'000'000'000ull)};
    int op=shared ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE;
    return syscall(SYS_futex,reinterpret_cast<uint32_t*>(addr),op,expected,&ts,nullptr,0);
}

inline long futexWake(std::atomic<uint32_t>* addr, bool shared=false, int n=INT_MAX) {
    int op=shared ? FUTEX_WAKE : FUTEX_WAKE_PRIVATE;
    return syscall(SYS_futex,reinterpret_cast<uint32_t*>(addr),op,n,nullptr,nullptr,0);
}

} // namespace daq
