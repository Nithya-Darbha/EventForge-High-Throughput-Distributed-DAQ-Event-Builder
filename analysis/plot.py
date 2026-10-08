#!/usr/bin/env python3
"""
Turns results/ into the plots in docs/img/ (+ a few numbers printed to the terminal).

    python3 analysis/plot.py                 # results/ -> docs/img/
    python3 analysis/plot.py --results r2 --out /tmp/plots

Only plots scenarios that have results, so you can run one scenario and plot it.
No dual y axes anywhere: two different units = two panels.
"""
import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

# fixed series colours (validated categorical palette, used in this order, never cycled)
C1 = "#2a78d6"  # blue   - BLOCK / spsc / p50 / main series
C2 = "#eb6834"  # orange - DROP / mutex / p95
C3 = "#1baf7a"  # aqua   - p99 / 3rd series
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e4e3df"
SHADE = "#f1efe8"

plt.rcParams.update({
    "figure.dpi": 110,
    "savefig.dpi": 150,
    "font.size": 10,
    "axes.edgecolor": INK2,
    "axes.labelcolor": INK2,
    "axes.titlesize": 11,
    "axes.titleweight": "bold",
    "axes.titlecolor": INK,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "xtick.color": INK2,
    "ytick.color": INK2,
    "legend.frameon": False,
    "lines.linewidth": 2,
    "lines.markersize": 6,
    "figure.facecolor": "white",
    "axes.facecolor": "white",
})


# ---------- loading ----------

def load_scenario(results, name):
    base = Path(results) / name
    idx = base / "index.json"
    if not idx.exists():
        return None
    runs = []
    for r in json.loads(idx.read_text())["runs"]:
        d = base / r["dir"]
        run = {"label": r["label"], "params": r["params"],
               "summary": json.loads((d / "summary.json").read_text()),
               "fronts": json.loads((d / "frontends.json").read_text()),
               "metrics": []}
        m = d / "metrics.jsonl"
        if m.exists():
            run["metrics"] = [json.loads(l) for l in m.read_text().splitlines() if l.strip()]
        runs.append(run)
    return runs


def series(metrics, key):
    return [m["t"] for m in metrics], [m[key] for m in metrics]


def param(run, key):
    for k, v in run["params"].items():
        if k.endswith(key):
            return v
    return None


def save(fig, out, name):
    out.mkdir(parents=True, exist_ok=True)
    fig.savefig(out / name, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out / name}")


def shade(ax, windows):
    for a, b in windows:
        ax.axvspan(a, b, color=SHADE, zorder=0, lw=0)


def recovery_time(metrics, end_s, key="pool_frac", tol=None, base_window=(0.3, 0.95)):
    """
    time from end of a disturbance until `key` is back near its steady state level
    (median over base_window, before any disturbance) and deadtime is ~0,
    for 3 samples in a row. None = never recovered in the run
    """
    before = sorted(m[key] for m in metrics if base_window[0] < m["t"] < base_window[1])
    if not before:
        return None
    base = before[len(before) // 2]
    limit = tol if tol is not None else max(base * 2, base + 0.02)
    ok = 0
    streak_start = None
    for m in metrics:
        if m["t"] < end_s:
            continue
        good = m[key] <= limit and m["deadtime"] < 0.01
        if good:
            if ok == 0:
                streak_start = m["t"]
            ok += 1
        else:
            ok = 0
        if ok == 3:
            # first sample of the good streak; resolution = sample period
            return max(streak_start - end_s, 0.0)
    return None


# ---------- plots ----------

def plot_rate_sweep(runs, out):
    runs = sorted(runs, key=lambda r: r["summary"]["offered_per_s"])
    x = [r["summary"]["offered_per_s"] / 1e3 for r in runs]
    built = [r["summary"]["events_per_s"] / 1e3 for r in runs]
    dead = [100 * r["summary"]["deadtime_frac"] for r in runs]
    p50 = [r["summary"]["latency_e2e_us"]["p50"] for r in runs]
    p95 = [r["summary"]["latency_e2e_us"]["p95"] for r in runs]
    p99 = [r["summary"]["latency_e2e_us"]["p99"] for r in runs]

    fig, ax = plt.subplots(1, 3, figsize=(14, 3.8))
    ax[0].plot(x, x, ls="--", color=INK2, lw=1, label="offered = built")
    ax[0].plot(x, built, marker="o", color=C1, label="built")
    ax[0].set(title="Throughput vs offered load", xlabel="offered trigger rate (kHz)", ylabel="events built (k/s)")
    ax[0].legend()

    ax[1].plot(x, dead, marker="o", color=C1)
    ax[1].set(title="Deadtime (BUSY throttling)", xlabel="offered trigger rate (kHz)", ylabel="triggers vetoed (%)")

    for ys, c, name in [(p50, C1, "p50"), (p95, C2, "p95"), (p99, C3, "p99")]:
        ax[2].plot(x, [y / 1e3 for y in ys], marker="o", color=c, label=name)
    ax[2].set_yscale("log")
    plain = FuncFormatter(lambda v, _: f"{v:g}")
    ax[2].yaxis.set_major_formatter(plain)
    ax[2].yaxis.set_minor_formatter(plain)
    ax[2].tick_params(axis="y", which="minor", labelsize=8)
    ax[2].set(title="Trigger -> stored latency", xlabel="offered trigger rate (kHz)", ylabel="latency (ms, log)")
    ax[2].legend()
    fig.suptitle("Builder capacity, 4 sources x 4 KB, no artificial sink delay", color=INK2, y=1.02, fontsize=10)
    save(fig, out, "rate_sweep.png")
    knee = max(built)
    print(f"  rate_sweep: max built rate {knee:.1f}k ev/s "
          f"({knee * 1e3 * 4 * 4096 / 1e6:.0f} MB/s payload)")


def plot_overload(runs, out):
    by = {"block": [], "drop": []}
    for r in runs:
        by[param(r, "policy")].append(r)
    for k in by:
        by[k].sort(key=lambda r: r["summary"]["offered_per_s"])

    fig, ax = plt.subplots(1, 3, figsize=(14, 3.8))
    for pol, c in [("block", C1), ("drop", C2)]:
        rs = by[pol]
        x = [r["summary"]["offered_per_s"] / 1e3 for r in rs]
        built = [r["summary"]["events_complete"] / r["summary"]["config"]["duration_s"] / 1e3 for r in rs]
        offered = [r["summary"]["triggers_issued"] + r["summary"]["triggers_vetoed"] for r in rs]
        lost = [100 * (o - r["summary"]["events_complete"]) / o for o, r in zip(offered, rs)]
        p99 = [r["summary"]["latency_e2e_us"]["p99"] / 1e3 for r in rs]
        ax[0].plot(x, built, marker="o", color=c, label=pol.upper())
        ax[1].plot(x, lost, marker="o", color=c, label=pol.upper())
        ax[2].plot(x, p99, marker="o", color=c, label=pol.upper())
    ax[0].axhline(14.2, ls="--", lw=1, color=INK2)
    ax[0].text(ax[0].get_xlim()[0] + 0.5, 14.6, "sink capacity", color=INK2, fontsize=9)
    ax[0].set(title="Complete events built", xlabel="offered trigger rate (kHz)", ylabel="complete events (k/s)")
    ax[1].set(title="Triggers that never became a complete event",
              xlabel="offered trigger rate (kHz)", ylabel="% of offered triggers")
    ax[2].set(title="p99 latency of complete events", xlabel="offered trigger rate (kHz)", ylabel="latency (ms)")
    for a in ax:
        a.legend()
    fig.suptitle("BLOCK (busy -> trigger veto) vs DROP (frontend throws frags away), slow sink",
                 color=INK2, y=1.02, fontsize=10)
    save(fig, out, "block_vs_drop.png")


def plot_credits(runs, out):
    runs = sorted(runs, key=lambda r: param(r, "credits"))
    cr = [param(r, "credits") for r in runs]
    p50 = [r["summary"]["latency_e2e_us"]["p50"] / 1e3 for r in runs]
    tput = [r["summary"]["events_per_s"] for r in runs]
    # little's law: events in flight ~= credits (each source can have `credits` frags out)
    pred = [c / t * 1e3 for c, t in zip(cr, tput)]

    fig, ax = plt.subplots(1, 2, figsize=(10, 3.8))
    ax[0].plot(cr, p50, marker="o", color=C1, label="measured p50")
    ax[0].plot(cr, pred, ls="--", color=C2, label="Little's law: credits / throughput")
    ax[0].set_xscale("log", base=2)
    ax[0].set(title="Latency under overload is set by the credit window",
              xlabel="credits per source", ylabel="latency (ms)")
    ax[0].legend()
    ax[1].plot(cr, [t / 1e3 for t in tput], marker="o", color=C1)
    ax[1].set_xscale("log", base=2)
    ax[1].set_ylim(0, max(tput) / 1e3 * 1.2)
    ax[1].set(title="Throughput", xlabel="credits per source", ylabel="events built (k/s)")
    save(fig, out, "credits_latency.png")


BURSTS = [(1.0, 1.3), (2.5, 2.8), (4.0, 4.3)]


def timeseries_column(axs, m, windows, show_offered=True):
    t, built = series(m, "ev_rate")
    _, trig = series(m, "trig_rate")
    _, veto = series(m, "veto_rate")
    offered = [a + b for a, b in zip(trig, veto)]
    for a in axs:
        shade(a, windows)
    if show_offered:
        axs[0].plot(t, [o / 1e3 for o in offered], color=INK2, lw=1, ls="--", label="offered")
    axs[0].plot(t, [b / 1e3 for b in built], color=C1, label="built")
    axs[0].set_ylabel("events (k/s)")
    axs[0].legend(loc="upper right")
    _, dead = series(m, "deadtime")
    axs[1].plot(t, [100 * d for d in dead], color=C1)
    axs[1].set_ylabel("deadtime (%)")
    axs[1].set_ylim(-5, 105)
    _, pool = series(m, "pool_frac")
    axs[2].plot(t, [100 * p for p in pool], color=C1)
    axs[2].set_ylabel("buffer pool used (%)")
    _, p99 = series(m, "lat_p99_us")
    # no events finished in that interval -> no latency, leave a gap instead of a fake 0
    p99 = [p / 1e3 if b > 0 else float("nan") for p, b in zip(p99, built)]
    axs[3].plot(t, p99, color=C1)
    axs[3].set_ylabel("p99 latency (ms)")
    axs[3].set_xlabel("time since run start (s)")


def plot_burst(runs, out):
    runs = sorted(runs, key=lambda r: param(r, "policy"))
    fig, ax = plt.subplots(4, len(runs), figsize=(6.5 * len(runs), 9), sharex=True)
    if len(runs) == 1:
        ax = [[a] for a in ax]
    for col, r in enumerate(runs):
        col_axes = [ax[row][col] for row in range(4)]
        timeseries_column(col_axes, r["metrics"], BURSTS)
        col_axes[0].set_title(f"{param(r, 'policy').upper()}: 300 ms bursts to 30 kHz (capacity ~14 kHz)")
        if param(r, "policy") == "block":
            rec = [recovery_time(r["metrics"], b) for _, b in BURSTS]
            print("  burst/block recovery after each burst (s):", [None if x is None else round(x, 2) for x in rec])
        else:
            _, inc = series(r["metrics"], "incomplete")
            col_axes[1].set_title("DROP never vetoes, incomplete events: %d" % inc[-1], fontsize=9, color=INK2)
    save(fig, out, "burst.png")


def plot_slow_consumer(runs, out):
    r = runs[0]
    m = r["metrics"]
    win = [(2.0, 3.0)]
    fig, ax = plt.subplots(4, 1, figsize=(8, 9), sharex=True)
    timeseries_column(ax, m, win)
    rec = recovery_time(m, 3.0, base_window=(0.3, 1.9))
    ax[0].set_title("Consumer slowdown: sinks 10x slower for 1 s (shaded)")
    if rec is not None:
        for a in ax:
            a.axvline(3.0 + rec, color=INK2, lw=1, ls=":")
        ax[2].text(3.0 + rec + 0.05, ax[2].get_ylim()[1] * 0.8, f"recovered {rec * 1e3:.0f} ms after", color=INK2, fontsize=9)
    print(f"  slow_consumer: recovery {None if rec is None else round(rec, 3)} s after the slowdown ended")
    save(fig, out, "slow_consumer.png")


def plot_producer_failure(runs, out):
    runs = sorted(runs, key=lambda r: bool(param(r, "degraded")))
    fig, ax = plt.subplots(3, len(runs), figsize=(6.5 * len(runs), 7.5), sharex=True, sharey="row")
    if len(runs) == 1:
        ax = [[a] for a in ax]
    for col, r in enumerate(runs):
        m = r["metrics"]
        t = [x["t"] for x in m]

        def rate(key):
            v = [x[key] for x in m]
            out_ = [0.0]
            for i in range(1, len(v)):
                dt = t[i] - t[i - 1]
                out_.append((v[i] - v[i - 1]) / dt if dt > 0 else 0)
            return out_

        a0, a1, a2 = ax[0][col], ax[1][col], ax[2][col]
        for a in (a0, a1, a2):
            shade(a, [(2.0, 3.5)])
        complete = rate("complete")
        degraded = rate("degraded")
        full = [c - d for c, d in zip(complete, degraded)]
        a0.plot(t, [x / 1e3 for x in full], color=C1, label="complete, all 4 sources")
        a0.plot(t, [x / 1e3 for x in degraded], color=C2, label="complete, degraded (3 sources)")
        a0.set_ylabel("events (k/s)")
        a0.legend(loc="center left", fontsize=8)
        a1.plot(t, [x / 1e3 for x in rate("incomplete")], color=C1)
        a1.set_ylabel("incomplete (timed out, k/s)")
        a2.plot(t, [100 * x["deadtime"] for x in m], color=C1)
        a2.set_ylabel("deadtime (%)")
        a2.set_xlabel("time since run start (s)")
        mode = "degraded mode ON" if param(r, "degraded") else "normal mode"
        s = r["summary"]
        a0.set_title(f"{mode}: incomplete {s['events_incomplete']}, deadtime {100 * s['deadtime_frac']:.1f}%")
    fig.suptitle("Frontend 1 SIGKILLed at 2.0 s, restarted at 3.5 s (shaded)", color=INK2, y=1.0, fontsize=10)
    save(fig, out, "producer_failure.png")


def plot_queue_compare(runs, out):
    rates = sorted({param(r, "rate") for r in runs})
    kinds = ["spsc", "mutex"]
    get = {(param(r, "queue"), param(r, "rate")): r for r in runs}
    fig, ax = plt.subplots(1, 2, figsize=(10, 3.8))
    w = 0.38
    for i, (k, c) in enumerate(zip(kinds, [C1, C2])):
        xs = [j + (i - 0.5) * w for j in range(len(rates))]
        p99 = [get[(k, rt)]["summary"]["latency_e2e_us"]["p99"] / 1e3 for rt in rates]
        cpu = []
        for rt in rates:
            s = get[(k, rt)]["summary"]
            n = max(s["events_complete"], 1)
            cpu.append(s["cpu_s"] / n * 1e6)
        ax[0].bar(xs, p99, w * 0.92, color=c, label=k)
        ax[1].bar(xs, cpu, w * 0.92, color=c, label=k)
    for a in ax:
        a.set_xticks(range(len(rates)), [f"{r // 1000}k" for r in rates])
        a.set_xlabel("offered trigger rate")
        a.legend()
        a.grid(axis="x", visible=False)
    ax[0].set(title="p99 latency", ylabel="ms")
    ax[1].set(title="Builder CPU per event", ylabel="us of CPU / event")
    fig.suptitle("Receiver -> shard queue: lock-free SPSC ring vs mutex + condvar", color=INK2, y=1.02, fontsize=10)
    save(fig, out, "queue_compare.png")


def faults_table(runs, out):
    r = runs[0]
    s = r["summary"]
    f = {x["source_id"]: x for x in r["fronts"]}
    ps = {x["id"]: x for x in s["per_source"]}
    rows = [
        ("loss (src 0)", f[0]["lost_injected"], ps[0]["net_transit_loss"], "net_transit_loss"),
        ("dup (src 1)", f[1]["dup_injected"], ps[1]["duplicates"], "duplicates"),
        ("reorder (src 2)", f[2]["reorder_injected"], ps[2]["reordered"], "reordered"),
        ("corrupt (src 3)", f[3]["corrupt_injected"], s["crc_errors"], "crc_errors"),
        ("incomplete events", f[0]["lost_injected"] + f[3]["corrupt_injected"], s["events_incomplete"],
         "events_incomplete (= loss + corrupt)"),
    ]
    lines = ["| fault | injected by frontend | counted by builder | builder counter |", "|---|---|---|---|"]
    for name, inj, got, key in rows:
        lines.append(f"| {name} | {inj} | {got} | `{key}` |")
    lines.append("")
    lines.append(f"verify_fail={s['verify_fail']}, pool_leaked={s['pool_leaked']}, "
                 f"events_complete={s['events_complete']} of {s['triggers_issued']} triggers")
    text = "\n".join(lines)
    print("  faults accounting (paste into README):\n" + text)


def baseline_table(runs):
    s = runs[0]["summary"]
    lat = s["latency_e2e_us"]
    print(f"  baseline: {s['events_complete']}/{s['triggers_issued']} complete, {s['mb_per_s']:.0f} MB/s, "
          f"p50 {lat['p50']:.0f} us p99 {lat['p99']:.0f} us, verify_fail {s['verify_fail']}, cpu {s['cpu_s']:.2f}s")
    print("  thread cpu:", s["thread_cpu_s"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=str(ROOT / "results"))
    ap.add_argument("--out", default=str(ROOT / "docs" / "img"))
    args = ap.parse_args()
    out = Path(args.out)

    todo = [
        ("baseline", lambda r: baseline_table(r)),
        ("rate_sweep", lambda r: plot_rate_sweep(r, out)),
        ("overload", lambda r: plot_overload(r, out)),
        ("credits", lambda r: plot_credits(r, out)),
        ("burst", lambda r: plot_burst(r, out)),
        ("slow_consumer", lambda r: plot_slow_consumer(r, out)),
        ("producer_failure", lambda r: plot_producer_failure(r, out)),
        ("queue_compare", lambda r: plot_queue_compare(r, out)),
        ("faults", lambda r: faults_table(r, out)),
    ]
    for name, fn in todo:
        runs = load_scenario(args.results, name)
        if runs is None:
            print(f"[{name}] no results, skipped")
            continue
        print(f"[{name}]")
        fn(runs)


if __name__ == "__main__":
    main()
