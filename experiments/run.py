#!/usr/bin/env python3
"""
Runs one experiment scenario (a yaml file): starts daq_builder + N daq_frontend,
does the scheduled faults (kill / restart a frontend), collects the results.

    python3 experiments/run.py experiments/scenarios/baseline.yaml
    python3 experiments/run.py experiments/scenarios/overload.yaml --set builder.duration_s=2

output: results/<scenario>/<run label>/{summary.json, metrics.jsonl, frontends.json, config.json}
        results/<scenario>/index.json  (one line per run, for plot.py)

C++ does all the work, this only launches processes and moves files around.
"""
import argparse
import copy
import itertools
import json
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]


def to_args(d):
    # {"duration_s": 3, "degraded": True} -> ["--duration-s", "3", "--degraded"]
    out = []
    for k, v in d.items():
        flag = "--" + str(k).replace("_", "-")
        if isinstance(v, bool):
            if v:
                out.append(flag)
        elif v is not None:
            out += [flag, str(v)]
    return out


def set_dotted(cfg, key, value):
    # "builder.rate" -> cfg["builder"]["rate"]
    parts = key.split(".")
    d = cfg
    for p in parts[:-1]:
        d = d.setdefault(p, {})
    d[parts[-1]] = value


def parse_value(s):
    try:
        return yaml.safe_load(s)
    except yaml.YAMLError:
        return s


def expand_sweep(cfg):
    # sweep: {builder.rate: [a, b], frontend.policy: [x, y]} -> every combination
    sweep = cfg.get("sweep") or {}
    if not sweep:
        return [("default", cfg, {})]
    keys = list(sweep.keys())
    runs = []
    for combo in itertools.product(*[sweep[k] for k in keys]):
        c = copy.deepcopy(cfg)
        params = {}
        for k, v in zip(keys, combo):
            set_dotted(c, k, v)
            params[k] = v
        label = "_".join(f"{k.split('.')[-1]}={v}" for k, v in params.items())
        runs.append((label, c, params))
    return runs


class RunHandle:
    """one builder + its frontends"""

    def __init__(self, cfg, build_dir, port, outdir):
        self.cfg = cfg
        self.build_dir = Path(build_dir)
        self.port = port
        self.outdir = outdir
        self.fronts = {}       # source id -> current Popen
        self.front_logs = []   # (source id, Popen) every process we ever started
        self.started_at = None
        self.started_evt = threading.Event()
        self.builder_err = []

    def frontend_cmd(self, sid):
        f = dict(self.cfg.get("frontend") or {})
        per = (self.cfg.get("frontends") or {}).get(sid) or {}
        f.update(per)
        return [str(self.build_dir / "daq_frontend"), "--source-id", str(sid), "--port", str(self.port)] + to_args(f)

    def start_frontend(self, sid):
        p = subprocess.Popen(self.frontend_cmd(sid), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        self.fronts[sid] = p
        self.front_logs.append((sid, p))

    def read_builder_err(self):
        for line in self.builder.stderr:
            self.builder_err.append(line)
            if "run started" in line and not self.started_evt.is_set():
                self.started_at = time.monotonic()
                self.started_evt.set()

    def run(self):
        b = dict(self.cfg["builder"])
        b["port"] = self.port
        b["metrics_out"] = str(self.outdir / "metrics.jsonl")
        cmd = [str(self.build_dir / "daq_builder")] + to_args(b)
        self.builder = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        threading.Thread(target=self.read_builder_err, daemon=True).start()
        time.sleep(0.1)
        for sid in range(int(b.get("sources", 4))):
            self.start_frontend(sid)

        if not self.started_evt.wait(15):
            self.builder.kill()
            raise RuntimeError("builder never started the run:\n" + "".join(self.builder_err))

        # scheduled faults, times are relative to t0 (= run start + start delay)
        t0 = self.started_at + float(b.get("start_delay_ms", 200)) / 1000
        for ev in sorted(self.cfg.get("events") or [], key=lambda e: e["at_s"]):
            delay = t0 + float(ev["at_s"]) - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            if "kill" in ev:
                sid = int(ev["kill"])
                print(f"   t={ev['at_s']}s: SIGKILL frontend {sid}", file=sys.stderr)
                self.fronts[sid].send_signal(signal.SIGKILL)
            if "restart" in ev:
                sid = int(ev["restart"])
                print(f"   t={ev['at_s']}s: restart frontend {sid}", file=sys.stderr)
                self.start_frontend(sid)

        timeout = float(b.get("duration_s", 5)) + 60
        out, _ = self.builder.communicate(timeout=timeout)
        front_stats = []
        for sid, p in self.front_logs:
            try:
                _, err = p.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                _, err = p.communicate()
            lines = [l for l in (err or "").splitlines() if l.startswith("{")]
            st = json.loads(lines[-1]) if lines else {"source_id": sid, "rc": p.returncode, "no_stats": True}
            front_stats.append(st)
        if not out.strip():
            raise RuntimeError("builder printed no summary:\n" + "".join(self.builder_err))
        return json.loads(out), front_stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario")
    ap.add_argument("--build-dir", default=str(ROOT / "build"))
    ap.add_argument("--out", default=str(ROOT / "results"))
    ap.add_argument("--set", action="append", default=[], help="override, e.g. builder.rate=20000")
    ap.add_argument("--port", type=int, default=9300)
    args = ap.parse_args()

    cfg = yaml.safe_load(Path(args.scenario).read_text())
    for s in args.set:
        k, v = s.split("=", 1)
        set_dotted(cfg, k, parse_value(v))
    name = cfg.get("name") or Path(args.scenario).stem
    base = Path(args.out) / name
    base.mkdir(parents=True, exist_ok=True)

    index = []
    runs = expand_sweep(cfg)
    for i, (label, rcfg, params) in enumerate(runs):
        outdir = base / label
        outdir.mkdir(parents=True, exist_ok=True)
        print(f"[{name}] run {i + 1}/{len(runs)}: {label}", file=sys.stderr)
        port = args.port + (i % 200)
        summary, fronts = RunHandle(rcfg, args.build_dir, port, outdir).run()
        (outdir / "summary.json").write_text(json.dumps(summary, indent=2))
        (outdir / "frontends.json").write_text(json.dumps(fronts, indent=2))
        (outdir / "config.json").write_text(json.dumps(rcfg, indent=2))
        lat = summary["latency_e2e_us"]
        print(f"      offered {summary['offered_per_s']:,.0f}/s -> built {summary['events_per_s']:,.0f}/s"
              f"  deadtime {summary['deadtime_frac']:.1%}  incomplete {summary['events_incomplete']}"
              f"  p50/p99 {lat['p50']:.0f}/{lat['p99']:.0f} us", file=sys.stderr)
        index.append({"label": label, "params": params, "dir": str(outdir.relative_to(base))})

    (base / "index.json").write_text(json.dumps({"name": name, "runs": index}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
