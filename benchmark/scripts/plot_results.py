#!/usr/bin/env python3
"""
Render charts from a benchmark results directory.

Reads the JSON files written by run-net.sh / run-net.bat and produces
grouped bar charts (PNG) for throughput, tail latency and connection rate.

Usage:
    python3 plot_results.py [RESULTS_DIR]

If RESULTS_DIR is omitted, the most recent benchmark/out/results/<timestamp>/
directory is used. Charts are written into <RESULTS_DIR>/charts/.

Filename conventions produced by the runners (the <proto>- prefix is
optional for backward compatibility with older, TCP-only result sets):
    {proto}-throughput-{st|mt}-c{1k|10k}-{64B|4K|64K}-{server}-r{run}.json
    {proto}-connrate-{st|mt}-{1k|10k}-{server}.json

A results directory may contain several protocols; charts are emitted
per protocol, e.g. tls_throughput_mt.png.
"""
import glob
import json
import os
import re
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SERVERS = ["xylem", "go", "rust"]
COLORS = {
    "xylem": "#2563eb",   # blue
    "go":    "#0891b2",   # cyan
    "rust":  "#ea580c",   # orange
}
PAYLOAD_ORDER = ["64B", "4K", "64K"]
PAYLOAD_BYTES = {"64B": 64, "4K": 4096, "64K": 65536}
CONN_ORDER = ["1k", "10k"]

TP_RE = re.compile(
    r"(?:(?P<proto>tcp|udp|tls)-)?throughput-(?P<mode>st|mt)-"
    r"c(?P<conns>\d+k?)-(?P<payload>\d+[BK])-"
    r"(?P<server>[a-z]+)-r(?P<run>\d+)\.json$"
)
CR_RE = re.compile(
    r"(?:(?P<proto>tcp|udp|tls)-)?connrate-(?P<mode>st|mt)-"
    r"(?P<conns>\d+k?)-(?P<server>[a-z]+)\.json$"
)


def find_results_dir():
    if len(sys.argv) > 1:
        return os.path.abspath(sys.argv[1])
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, "..", "out", "results"))
    dirs = sorted(
        d for d in glob.glob(os.path.join(root, "*")) if os.path.isdir(d)
    )
    if not dirs:
        sys.exit(f"no result directories found under {root}")
    return dirs[-1]


def load(results_dir):
    """Return (throughput, connrate) keyed by protocol then dimensions."""
    # raw_tp[proto][mode][(payload, conns)][server] -> [data, ...]
    raw_tp = defaultdict(
        lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(list))))
    # connrate[proto][mode][conns][server] -> conn/s
    connrate = defaultdict(lambda: defaultdict(lambda: defaultdict(dict)))

    for path in glob.glob(os.path.join(results_dir, "*.json")):
        name = os.path.basename(path)
        try:
            with open(path) as fh:
                data = json.load(fh)
        except (OSError, json.JSONDecodeError):
            continue

        m = TP_RE.match(name)
        if m:
            proto = m["proto"] or "tcp"
            key = (m["payload"], m["conns"])
            raw_tp[proto][m["mode"]][key][m["server"]].append(data)
            continue

        m = CR_RE.match(name)
        if m:
            proto = m["proto"] or "tcp"
            cps = data.get("connects_per_sec") or data.get("connrate") or 0
            connrate[proto][m["mode"]][m["conns"]][m["server"]] = cps

    # average runs -> tp[proto][mode][key][server]
    tp = defaultdict(lambda: defaultdict(lambda: defaultdict(dict)))
    for proto, modes in raw_tp.items():
        for mode, scenarios in modes.items():
            for key, servers in scenarios.items():
                for srv, runs in servers.items():
                    def avg(field):
                        vals = [r.get(field, 0) for r in runs if r.get(field)]
                        return sum(vals) / len(vals) if vals else 0
                    tp[proto][mode][key][srv] = {
                        "throughput": avg("throughput_msg_per_sec"),
                        "p50": avg("latency_p50_us"),
                        "p99": avg("latency_p99_us"),
                        "max": avg("latency_max_us"),
                    }
    return tp, connrate


def scenario_keys(tp_mode):
    keys = list(tp_mode.keys())
    keys.sort(key=lambda k: (PAYLOAD_ORDER.index(k[0]) if k[0] in PAYLOAD_ORDER else 99,
                             CONN_ORDER.index(k[1]) if k[1] in CONN_ORDER else 99))
    return keys


def grouped_bar(ax, categories, series_data, ylabel, title, logy=False):
    n_series = len(series_data)
    n_cat = len(categories)
    width = 0.8 / max(n_series, 1)
    x = np.arange(n_cat)
    for i, (srv, vals) in enumerate(series_data):
        offset = (i - (n_series - 1) / 2) * width
        ax.bar(x + offset, vals, width, label=srv, color=COLORS.get(srv, None))
    ax.set_xticks(x)
    ax.set_xticklabels(categories)
    ax.set_ylabel(ylabel)
    ax.set_title(title, fontweight="bold")
    if logy:
        ax.set_yscale("log")
    ax.grid(axis="y", linestyle="--", alpha=0.4)
    ax.legend(fontsize=8, ncol=len(series_data))


def present_servers(tp_mode):
    seen = []
    for servers in tp_mode.values():
        for srv in servers:
            if srv not in seen:
                seen.append(srv)
    return [s for s in SERVERS if s in seen] + [s for s in seen if s not in SERVERS]


def plot_throughput(tp, out_dir, proto):
    for mode in ("st", "mt"):
        if mode not in tp:
            continue
        keys = scenario_keys(tp[mode])
        cats = [f"{p}\nc{c}" for (p, c) in keys]
        servers = present_servers(tp[mode])

        fig, (ax_tp, ax_lat) = plt.subplots(2, 1, figsize=(11, 9))

        tp_series = []
        for srv in servers:
            vals = [tp[mode][k].get(srv, {}).get("throughput", 0) / 1000 for k in keys]
            tp_series.append((srv, vals))
        grouped_bar(ax_tp, cats, tp_series, "throughput (k msg/s)",
                    f"{proto.upper()} throughput — {mode.upper()}")

        lat_series = []
        for srv in servers:
            vals = [tp[mode][k].get(srv, {}).get("p99", 0) / 1000 for k in keys]
            lat_series.append((srv, vals))
        grouped_bar(ax_lat, cats, lat_series, "p99 latency (ms, log)",
                    f"{proto.upper()} p99 latency — {mode.upper()}", logy=True)

        fig.tight_layout()
        out = os.path.join(out_dir, f"{proto}_throughput_{mode}.png")
        fig.savefig(out, dpi=130)
        plt.close(fig)
        print(f"  wrote {out}")


def plot_connrate(connrate, out_dir, proto):
    if not connrate:
        return
    modes = [m for m in ("st", "mt") if m in connrate]
    cats, col_keys = [], []
    for mode in modes:
        for conns in CONN_ORDER:
            if conns in connrate[mode]:
                cats.append(f"{mode.upper()}\nc{conns}")
                col_keys.append((mode, conns))
    servers = []
    for mode, conns in col_keys:
        for srv in connrate[mode][conns]:
            if srv not in servers:
                servers.append(srv)
    servers = [s for s in SERVERS if s in servers]

    fig, ax = plt.subplots(figsize=(10, 5))
    series = []
    for srv in servers:
        vals = [connrate[m][c].get(srv, 0) / 1000 for (m, c) in col_keys]
        series.append((srv, vals))
    grouped_bar(ax, cats, series, "conn/s (k)", f"{proto.upper()} connection rate")
    fig.tight_layout()
    out = os.path.join(out_dir, f"{proto}_connrate.png")
    fig.savefig(out, dpi=130)
    plt.close(fig)
    print(f"  wrote {out}")


def plot_throughput_vs_payload(tp, out_dir, proto):
    """Throughput as a function of payload size — small-msg vs bandwidth bound."""
    modes = [m for m in ("st", "mt") if m in tp]
    if not modes:
        return
    fig, axes = plt.subplots(len(modes), len(CONN_ORDER),
                             figsize=(12, 4.5 * len(modes)), squeeze=False)
    for r, mode in enumerate(modes):
        servers = present_servers(tp[mode])
        for c, conns in enumerate(CONN_ORDER):
            ax = axes[r][c]
            payloads = [p for p in PAYLOAD_ORDER if (p, conns) in tp[mode]]
            xs = [PAYLOAD_BYTES[p] for p in payloads]
            for srv in servers:
                ys = [tp[mode][(p, conns)].get(srv, {}).get("throughput", 0) / 1000
                      for p in payloads]
                ax.plot(xs, ys, marker="o", label=srv, color=COLORS.get(srv))
            ax.set_xscale("log", base=2)
            ax.set_yscale("log")
            ax.set_xticks(xs)
            ax.set_xticklabels(payloads)
            ax.set_xlabel("payload")
            ax.set_ylabel("throughput (k msg/s, log)")
            ax.set_title(f"{mode.upper()}  c{conns}", fontweight="bold")
            ax.grid(True, which="both", linestyle="--", alpha=0.4)
            ax.legend(fontsize=8)
    fig.suptitle(f"{proto.upper()} throughput vs payload size", fontweight="bold")
    fig.tight_layout()
    out = os.path.join(out_dir, f"{proto}_throughput_vs_payload.png")
    fig.savefig(out, dpi=130)
    plt.close(fig)
    print(f"  wrote {out}")


def main():
    results_dir = find_results_dir()
    out_dir = os.path.join(results_dir, "charts")
    os.makedirs(out_dir, exist_ok=True)
    print(f"results: {results_dir}")
    tp, connrate = load(results_dir)
    if not tp and not connrate:
        sys.exit("no parseable benchmark JSON found")
    protos = sorted(set(tp) | set(connrate))
    for proto in protos:
        plot_throughput(tp.get(proto, {}), out_dir, proto)
        plot_throughput_vs_payload(tp.get(proto, {}), out_dir, proto)
        plot_connrate(connrate.get(proto, {}), out_dir, proto)
    print(f"charts -> {out_dir}")


if __name__ == "__main__":
    main()
