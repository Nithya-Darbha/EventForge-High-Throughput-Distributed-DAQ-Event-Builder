# EventForge

A high-throughput **DAQ event builder with explicit backpressure**, written in C++20 for Linux.
It is modelled on the data acquisition chain of a particle physics experiment: several detector
frontends read out fragments of the same collision ("event"), and an event builder has to put the
pieces back together, at rate, without losing track of anything, and behave predictably when
something downstream can't keep up.

Everything runs on one machine: frontends are separate processes, the builder is one process with a
thread pipeline, and Python is only used to launch experiments and plot results.

```
                     TTC shared memory (trigger + BUSY)
            +---------------------------------------------------+
            |                                                   v
  frontend 0 --+                                    +--> shard 0 --+
  frontend 1 --+--TCP--> receiver (epoll) --SPSC----+              +--> done queue --> sink x M
  frontend N --+  <-------- credits ----------------+--> shard K --+                       |
                     ^                                                                    |
                     +-------------------- buffer slots + credits released <--------------+
```

## What it does

- **Event building**: fragments are matched by `eventId` across all sources in sharded assemblers
  (`eventId % shards`, one thread each, no locks on the event map). Incomplete events time out.
- **Central trigger with BUSY throttling**: a trigger generator writes triggers into a shared memory
  segment that every frontend reads (like the LHC TTC system). A frontend that can't keep up raises a
  BUSY bit and the trigger is **vetoed**. Vetoed triggers are counted as **deadtime**.
- **Credit based flow control**: each frontend may only have `credits` fragments in flight. Credits
  come back when the builder is done with a fragment, so memory and latency are bounded end to end.
- **Two overload policies** to compare: `BLOCK` (raise BUSY and wait, nothing is lost) and `DROP`
  (throw fragments away at the frontend, no deadtime).
- **Bounded everything**: preallocated buffer pool for payloads, bounded SPSC rings and queues
  between threads. No unbounded buffering anywhere.
- **Fault injection**: transit loss, duplicates, reordering, payload corruption (CRC32C), frontend
  crash (`_exit` or SIGKILL from the orchestrator), frontend restart mid-run, stuck frontend,
  consumer slowdown, trigger bursts.
- **Exact accounting**: every injected fault is classified and counted by the builder (seq gap vs
  source drop vs duplicate vs reorder vs CRC error), and the tests check the numbers match exactly.
- **Degraded mode**: when a source dies, events can complete without it instead of timing out.
- **Observability**: per-thread counters on their own cache lines, log-linear latency histograms,
  a sampler writing `metrics.jsonl` every 50 to 100 ms, per-thread CPU from `/proc`.

## Results

Numbers below are from an **Apple Silicon Mac, inside Docker (OrbStack's Linux VM, ARM64)**, with the
builder, 4 frontends and the trigger all on the same machine and 4 KB fragments. Also tested on a
2 vCPU x86 Linux VM. `make bench` regenerates everything, results will differ per machine. Getting
it to behave inside the Mac VM took some work, see [NOTES.md](NOTES.md).

| | |
|---|---|
| Baseline (4 sources, 10 kHz, 4 KB) | 50,000 / 50,000 events complete, every payload byte verified, trigger to stored latency p50 **45 µs**, p99 **78 µs** |
| Builder throughput | **79k events/s, ~1.3 GB/s** of payload at 1.5% deadtime, highest rate tested, not saturated yet |
| Overload, BLOCK | throughput holds at sink capacity (19.8k/s), **0 incomplete events**, the excess becomes deadtime (34 to 51%) |
| Overload, DROP | at 1.5 to 2x capacity throughput **collapses to 0.6k to 1.2k events/s**, 96 to 98% of triggers never become a complete event |
| Bursts to 2x capacity | BLOCK keeps building at full capacity and is back to normal **~90 ms** after each burst. DROP builds almost nothing during each burst |
| Consumer 10x slower for 1 s | 0 events lost, deadtime ~75% during the slowdown, recovered **~60 ms** after |
| Producer killed for 1.5 s | normal mode: 3,413 incomplete + 13.8% deadtime. degraded mode: **1 incomplete, 0% deadtime** |
| Fault accounting | injected loss / dup / reorder / corrupt counted **exactly** by the builder (table below) |

### Throughput and latency vs load
![rate sweep](docs/img/rate_sweep.png)

### BLOCK vs DROP under overload
![block vs drop](docs/img/block_vs_drop.png)

DROP looks attractive (no deadtime) but it collapses. Each frontend drops on its own, so different
sources keep fragments for *different* events. Those partial events then sit in the builder until the
timeout, holding buffers and credits, so the sources run out of credits again and drop more. Almost
no event ever gets all its fragments. This is the reason real DAQ systems throttle the trigger
centrally (BUSY) instead of letting each readout board drop data.

### Bursts: BLOCK rides through, DROP falls over
![burst](docs/img/burst.png)

During each burst BLOCK keeps building at full sink capacity (the excess becomes deadtime) and is back
to normal about 90 ms after the burst (the metrics are sampled every 50 ms, so that is the resolution). DROP builds close to nothing during the burst, exactly when
the data matters most, and recovers once the load is back below capacity.

An earlier version showed DROP *never* recovering after the first burst. That turned out to be partly
my own credit loop: credits only went back when the receiver woke up on its 1 ms epoll timeout. With
credit return made event driven (eventfd, see NOTES.md) DROP recovers, but the collapse during
overload is real.

### Latency is set by the credit window (Little's law)
![credits](docs/img/credits_latency.png)

Under overload the queueing delay is `in flight / throughput`, and the credit window is what caps
"in flight". Measured p50 follows `credits / throughput` from 16 to 512 credits (0.8 ms to 23.6 ms),
throughput stays flat at ~19.7k/s. So the credit size is a direct latency knob.

### Consumer slowdown and producer failure
![slow consumer](docs/img/slow_consumer.png)
![producer failure](docs/img/producer_failure.png)

### Fault accounting (faults.yaml)

| fault | injected by frontend | counted by builder | builder counter |
|---|---|---|---|
| loss (src 0) | 100 | 100 | `net_transit_loss` |
| dup (src 1) | 111 | 111 | `duplicates` |
| reorder (src 2) | 99 | 99 | `reordered` |
| corrupt (src 3) | 34 | 34 | `crc_errors` |
| incomplete events | 134 | 134 | `events_incomplete` (= loss + corrupt) |

### Lock-free SPSC vs mutex queue
![queue compare](docs/img/queue_compare.png)

Honest result: at these rates the receiver to shard queue is not the bottleneck, so the lock-free
ring doesn't clearly win. Same throughput, the ring has a lower p99 at 20k and 40k (135 vs 250 µs,
459 vs 672 µs), the mutex queue at 60k. Most of the CPU goes to the receiver (syscalls, memcpy,
CRC), and inside the Mac VM to the trigger thread, which spins to keep exact timing (see NOTES.md).
The ring only starts to matter when the queue itself is hot.

## Build and run

Linux only (epoll, futex, `/dev/shm`). GCC 12+ or Clang 15+, CMake 3.20+.
GoogleTest is used from the system if present, otherwise fetched by CMake.

```sh
cmake -S . -B build
cmake --build build -j
./build/unit_tests && ./build/e2e_tests
```

On a Mac, use the Dockerfile:

```sh
docker build -t eventforge .
docker run --rm -it -v "$PWD/results:/src/results" -v "$PWD/docs:/src/docs" eventforge
# inside: make test, make bench ...
```

Run one experiment, then plot:

```sh
pip install pyyaml matplotlib
python3 experiments/run.py experiments/scenarios/overload.yaml
python3 analysis/plot.py            # results/ -> docs/img/*.png
```

Or by hand:

```sh
./build/daq_builder --sources 2 --rate 10000 --duration-s 5 --metrics-out m.jsonl &
./build/daq_frontend --source-id 0 &
./build/daq_frontend --source-id 1 --loss 0.01
```

The builder prints a JSON summary on stdout at the end of the run.

### Main knobs

| builder | |
|---|---|
| `--sources N` | number of frontends (max 64) |
| `--rate HZ` / `--rate-profile "t:hz,..."` | trigger rate, or a piecewise rate for bursts |
| `--credits N` | fragments in flight per source |
| `--shards K` / `--sinks M` | assembler / sink threads |
| `--queue spsc\|mutex` | receiver to shard queue type |
| `--timeout-ms` | incomplete event timeout |
| `--degraded` | complete events without dead sources |
| `--sink-delay-us`, `--slow-at-s/--slow-for-s/--slow-delay-us` | sink cost, consumer slowdown fault |
| `--verify` | check every payload byte at the sink |
| `--spin-us N` | override the measured sleep/spin split (default: calibrated at startup) |

| frontend | |
|---|---|
| `--policy block\|drop` | what to do without credits |
| `--payload-min/--payload-max` | fragment size range |
| `--derand N` | local buffer depth before raising BUSY |
| `--loss --dup --reorder --corrupt P` | fault probabilities |
| `--die-at-s T`, `--stall-at-s T --stall-for-s D` | crash / freeze at time T |

## Experiments

| scenario | what it shows |
|---|---|
| `baseline` | sanity, everything complete and verified |
| `rate_sweep` | builder capacity, latency vs load, where BUSY kicks in |
| `overload` | BLOCK vs DROP with a slow consumer |
| `credits` | latency vs credit window (Little's law) |
| `burst` | bursts above capacity, recovery time |
| `slow_consumer` | sinks 10x slower for 1 s |
| `producer_failure` | frontend SIGKILL + restart, normal vs degraded mode |
| `faults` | exact accounting of every fault type |
| `queue_compare` | SPSC ring vs mutex queue |

## Repo layout

```
include/daq/      wire format, crc32c, histogram, queues, buffer pool, TTC shared memory, metrics
src/common/       implementations of the above
src/frontend/     frontend process: trigger input + BUSY, fault injector, main loop
src/builder/      receiver, shards (assembler + sweeper), sinks, trigger generator, main
tests/            unit tests + end to end tests that run the real binaries
experiments/      run.py + scenario yaml files
analysis/         plot.py
docs/             architecture.md, plots
```

More detail: [docs/architecture.md](docs/architecture.md) for how it works, [NOTES.md](NOTES.md)
for the design decisions and tradeoffs.

## Not in scope

Multi-node deployment, retransmission (lost fragments are counted, never recovered), persistent
storage, consensus. The builder is a single process, the "network" is loopback TCP.
