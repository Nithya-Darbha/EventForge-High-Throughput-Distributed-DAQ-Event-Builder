# Design notes

Why things are the way they are, and what I'd say if someone asks.

## TCP, not UDP

Loss is injected at the application level (the frontend gives a fragment a `seq` and then doesn't
send it). With UDP I would have had to build a reliability layer first, which is a different project.
The LHC experiments' event builders run over reliable transports too (TCP or RDMA), so this matches.
Downside: TCP has its own flow control, so a slow builder would also slow the frontend through the
socket buffer. Credits sit on top so the backpressure is explicit and measurable instead of hidden in
kernel buffers (phase 4 without credits showed exactly that: p99 went from 0.2 ms to 300 ms at
overload with nobody noticing anything was wrong).

## Push with credits (not pull)

Frontends push, the builder hands out credits. The alternative is pull (the builder asks for event
`n` from everyone, roughly what ATLAS did). Pull makes event ordering easier but adds a request round
trip per event. Credits are cumulative (`sent < limit`) like a TCP window, so a duplicated or late
CREDIT message can't break anything.

## Central trigger + BUSY instead of local throttling

First version: each frontend just waited when it had no credits. Problem: frontends desync, one
waits and skips triggers while the others don't, so their fragments are for different events.
Real DAQs solve this with a central trigger and a BUSY line (TTS in CMS): any readout that can't
take more data raises BUSY and the trigger stops for everyone, so all sources stay on the same
events. Here: the TTC is shared memory, BUSY is one bit per source in an atomic `uint64_t`, the
generator vetoes triggers while any bit is set. Deadtime = vetoed / offered.

The DROP results show why this matters: without coordination, goodput collapses during overload
(96 to 99% of triggers never become a complete event). In the first version it even stayed collapsed
after a burst ended, but part of that was my slow credit return (see the VM section below), after
fixing that it recovers once the load drops.

Possible fix for DROP if I ever want it: make drops coordinated (drop whole events, decided centrally
or by a deterministic rule on `eventId`), or evict partial events early when the pool is under
pressure so credits come back faster.

## Why credits bound latency

Under overload the pipeline is full, so by Little's law `latency = in flight / throughput`. In flight
is capped by the credit window. Measured p50 tracks `credits / throughput` from 16 to 512 credits.
So the credit window is the knob between "absorbs bigger bursts" and "lower latency".

## Sharded assemblers, no locks

`eventId % K` decides the shard. Every fragment of an event goes to the same shard, so a shard owns
its `unordered_map` alone and never locks it. The receiver is the only producer for each shard inbox
and the shard the only consumer, which is why an SPSC ring fits.

## SPSC ring vs mutex queue

The ring: head and tail on separate cache lines, each side caches the other index and only reloads it
when the ring looks full/empty, acquire/release only. When empty the consumer sleeps on a futex and
the producer only makes a syscall if it is actually asleep. Result: no big difference at these rates,
the queue isn't the bottleneck (the receiver thread is: syscalls, memcpy, CRC). I kept both behind
`--queue` and wrote that up instead of pretending lock free automatically wins.

## Buffer pool

Payload bytes are copied once from the socket buffer into a preallocated slot, after that only the
slot index moves. Nothing on the hot path mallocs payload memory. Running out of slots is a second
backpressure point (receiver stops reading that socket). Event metadata (`std::vector` of fragment
refs, map nodes) still allocates, that would be the next thing to fix with an event object pool.

## Seq vs eventId

Two counters so the builder can tell *why* a fragment is missing: `seq` only counts fragments that
entered transit, `eventId` counts triggers. A `seq` gap = lost in transit, an `eventId` jump with no
`seq` gap = the frontend dropped it on purpose. A bitmap of the last 4096 seqs separates duplicates
from reordered fragments. The end to end test checks that the counts match what was injected,
exactly.

## Connection generations

When a frontend restarts, fragments of its old connection can still be in the pipeline. If those
gave credits to the new connection, the new one could overrun its window. Each connection gets a
generation number, a released fragment only counts as credit if its generation is current.

## Latency measurement

`CLOCK_MONOTONIC` is the same clock for every process on a Linux box, so trigger time (set by the
generator) and commit time (sink) can be subtracted across processes. Latencies go into a log-linear
histogram (32 sub-buckets per power of two, under 3.2% error), one per sink thread, merged for reports.

## Timer slack

Linux adds up to 50 µs of slack to every sleep by default. The trigger generator and sinks set
`PR_SET_TIMERSLACK` to 1 ns, otherwise every 10 µs sleep would be a 60 µs sleep.

## Running it in a VM (OrbStack / Docker on a Mac): what broke and why

First run on an M-series Mac inside OrbStack: everything *correct* (all events built, fault
accounting exact) but p50 latency 5 ms instead of ~0.2 ms, and any scenario with a sink delay
built ~800 events/s instead of ~14k. Two separate causes:

1. **Short sleeps are slow in that VM.** A 100 µs `clock_nanosleep` took ~2.5 ms (2 sinks /
   800 events/s = 2.5 ms per "100 µs" sleep). The trigger thread was oversleeping too (it used
   0.05 s of CPU vs 1.0 s on bare Linux). Anything paced by a timer was off by 25x.
   Fixes:
   - `calibrateSpin()` at builder start measures how late a 50 µs sleep wakes up (p90) and
     `preciseSleepUntil()` sleeps most of the way then spins the rest. On bare Linux it spins
     ~30 µs, in the VM ~3-4 ms (= the trigger thread basically owns a core, which is what a
     real trigger board is anyway). Printed at startup and in the summary.
   - the SPSC shard inbox no longer polls with sleeps, the consumer sleeps on a futex and the
     receiver wakes it (eventcount pattern: `pushSeq` + `sleeping` flag + a fence each side).
     Wakeup is an event, not a timer, so it doesn't care about timer resolution.
   - credit return: sinks poke the receiver through an `eventfd` exactly once per credit batch,
     instead of the receiver noticing on its 1 ms epoll timeout. Also helped on bare Linux:
     baseline p50 188 -> 78 µs, overload capacity 14.1k -> 15.4k events/s.
2. **CRC32C was software on ARM.** I only wrote the SSE4.2 path. ARMv8 has `crc32cx` too
   (`__crc32cd`, checked at runtime with `getauxval(AT_HWCAP) & HWCAP_CRC32`). The receiver was
   CRC bound at 24k events/s. Tested by cross compiling with `aarch64-linux-gnu-g++` and running
   the tests + a full run under `qemu-aarch64`.

Also found while doing this: the TTC futex handshake used release/acquire on a store->load pair
(Dekker). Fine on x86 (locked RMW = full barrier), not guaranteed on ARM -> occasional missed
wakeup, caught by the 2 ms futex timeout. Now seq_cst fences on both sides.

## Things I'd do next

- event object pool + flat hash map for the partial events (remove remaining allocations)
- `io_uring` or `recvmmsg`-style batching on the receiver, it's the hottest thread
- multiple builder units with `eventId` routing (readout unit / builder unit split)
- coordinated DROP policy, see above
- run it on a bigger machine with frontends pinned to their own cores
