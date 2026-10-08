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

The DROP results show why this matters: without coordination, throughput collapses and stays
collapsed after a burst (metastable failure). That was not something I expected to be that strong.

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
when the ring looks full/empty, acquire/release only. Result: no real difference at these rates. The
queue isn't the bottleneck, and because the ring has no condvar the consumer polls with a backoff
(10 to 160 µs), which costs some latency at low rates. I kept both behind `--queue` and wrote that up
instead of pretending it was faster.

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

## Things I'd do next

- event object pool + flat hash map for the partial events (remove remaining allocations)
- `io_uring` or `recvmmsg`-style batching on the receiver, it's the hottest thread
- multiple builder units with `eventId` routing (readout unit / builder unit split)
- coordinated DROP policy, see above
- run it on a bigger machine with frontends pinned to their own cores
