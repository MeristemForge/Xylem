#!/usr/bin/env python3
"""
Xylem Benchmark Report Generator.

Reads JSON results from a benchmark run and produces PNG charts.
Usage: python3 report.py [results-dir]
  If no dir given, uses the latest timestamped directory in benchmark/results/.
"""

import json
import os
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

SCRIPT_DIR = Path(__file__).resolve().parent
BENCH_DIR = SCRIPT_DIR.parent
RESULTS_DIR = BENCH_DIR / "results"

COLORS = {
    "xylem": "#2196F3",
    "libuv": "#FF9800",
    "libevent": "#4CAF50",
    "libhv": "#9C27B0",
    "boost": "#F44336",
    "go": "#00BCD4",
    "rust": "#795548",
}

SERVERS = ["xylem", "libuv", "libevent", "libhv", "boost", "go", "rust"]


def find_results_dir():
    if len(sys.argv) > 1:
        return Path(sys.argv[1])
    dirs = sorted(RESULTS_DIR.glob("2*"), key=os.path.getmtime, reverse=True)
    if not dirs:
        print("no results found in", RESULTS_DIR)
        sys.exit(1)
    return dirs[0]


def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (json.JSONDecodeError, FileNotFoundError):
        return None


def setup_style():
    plt.rcParams.update({
        "figure.facecolor": "white",
        "axes.facecolor": "#f8f9fa",
        "axes.grid": True,
        "grid.alpha": 0.3,
        "font.size": 10,
        "axes.titlesize": 13,
        "axes.labelsize": 11,
    })


def plot_throughput_scaling(run_dir, out_dir):
    """Throughput vs connection count per server."""
    scales = [100, 500, 1000, 5000, 10000]
    scale_labels = ["100", "500", "1k", "5k", "10k"]

    fig, ax = plt.subplots(figsize=(10, 6))

    for server in SERVERS:
        throughputs = []
        valid_scales = []
        for s, label in zip(scales, scale_labels):
            path = run_dir / f"throughput-{server}-{label}.json"
            data = load_json(path)
            if data and "throughput_msg_per_sec" in data:
                throughputs.append(float(data["throughput_msg_per_sec"]))
                valid_scales.append(s)

        if throughputs:
            ax.plot(valid_scales, throughputs, "o-",
                    color=COLORS.get(server, "#666"),
                    label=server, linewidth=2, markersize=6)

    ax.set_xlabel("Connections")
    ax.set_xscale("log")
    ax.set_xticks(scales)
    ax.get_xaxis().set_major_formatter(ticker.ScalarFormatter())
    ax.set_ylabel("Throughput (msg/s)")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f"{x/1000:.0f}k"))
    ax.set_title("Throughput Scaling (echo ping-pong, 64B payload)")
    ax.legend(loc="best")
    fig.tight_layout()
    fig.savefig(out_dir / "throughput-scaling.png", dpi=150)
    plt.close(fig)
    print("  throughput-scaling.png")


def plot_latency(run_dir, out_dir):
    """P50 and P99 latency at c1k."""
    fig, ax = plt.subplots(figsize=(9, 5))

    servers_found = []
    p50s = []
    p99s = []

    for server in SERVERS:
        path = run_dir / f"throughput-{server}-1k.json"
        data = load_json(path)
        if data and "latency_p50_us" in data:
            servers_found.append(server)
            p50s.append(int(data["latency_p50_us"]))
            p99s.append(int(data["latency_p99_us"]))

    if not servers_found:
        plt.close(fig)
        return

    x = range(len(servers_found))
    width = 0.35
    bars1 = ax.bar([i - width / 2 for i in x], p50s, width,
                   label="P50", color="#2196F3", alpha=0.8)
    bars2 = ax.bar([i + width / 2 for i in x], p99s, width,
                   label="P99", color="#FF5722", alpha=0.8)

    ax.set_xticks(list(x))
    ax.set_xticklabels(servers_found)
    ax.set_ylabel("Latency (μs)")
    ax.set_title("Echo Latency at 1000 Connections")
    ax.legend()

    for bar in bars1:
        ax.annotate(f"{bar.get_height()}", xy=(bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    ha="center", va="bottom", fontsize=8)
    for bar in bars2:
        ax.annotate(f"{bar.get_height()}", xy=(bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    ha="center", va="bottom", fontsize=8)

    fig.tight_layout()
    fig.savefig(out_dir / "latency-c1k.png", dpi=150)
    plt.close(fig)
    print("  latency-c1k.png")


def plot_connrate(run_dir, out_dir):
    """Connection rate comparison."""
    fig, ax = plt.subplots(figsize=(8, 5))

    servers_found = []
    rates = []

    for server in SERVERS:
        path = run_dir / f"connrate-{server}.json"
        data = load_json(path)
        if data and "connects_per_sec" in data:
            servers_found.append(server)
            rates.append(float(data["connects_per_sec"]))

    if not servers_found:
        plt.close(fig)
        return

    colors = [COLORS.get(s, "#666") for s in servers_found]
    bars = ax.bar(servers_found, rates, color=colors, alpha=0.85)
    ax.set_ylabel("Connections / sec")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f"{x/1000:.0f}k"))
    ax.set_title("TCP Connection Rate")

    for bar in bars:
        ax.annotate(f"{bar.get_height()/1000:.1f}k",
                    xy=(bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    ha="center", va="bottom", fontsize=9)

    fig.tight_layout()
    fig.savefig(out_dir / "connrate.png", dpi=150)
    plt.close(fig)
    print("  connrate.png")


def plot_memory(run_dir, out_dir):
    """Memory (RSS) comparison at 10k connections."""
    fig, ax = plt.subplots(figsize=(8, 5))

    servers_found = []
    rss_values = []

    for server in SERVERS:
        path = run_dir / f"memory-{server}-10k.json"
        data = load_json(path)
        if data and "server_rss_kb" in data:
            servers_found.append(server)
            rss_values.append(int(data["server_rss_kb"]))

    if not servers_found:
        plt.close(fig)
        return

    colors = [COLORS.get(s, "#666") for s in servers_found]
    bars = ax.bar(servers_found, rss_values, color=colors, alpha=0.85)
    ax.set_ylabel("Server RSS (KB)")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(
        lambda x, _: f"{x/1024:.1f} MB" if x >= 1024 else f"{x:.0f} KB"))
    ax.set_title("Server Memory at 10k Idle Connections")

    for bar in bars:
        val = bar.get_height()
        label = f"{val/1024:.1f} MB" if val >= 1024 else f"{val} KB"
        ax.annotate(label, xy=(bar.get_x() + bar.get_width() / 2, val),
                    ha="center", va="bottom", fontsize=9)

    fig.tight_layout()
    fig.savefig(out_dir / "memory-10k.png", dpi=150)
    plt.close(fig)
    print("  memory-10k.png")


def plot_payload(run_dir, out_dir):
    """Throughput at different payload sizes."""
    sizes = [("64B", 64), ("1KB", 1024), ("4KB", 4096), ("64KB", 65536)]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))

    for server in SERVERS:
        msg_rates = []
        mbps_rates = []
        valid_labels = []
        for label, size in sizes:
            path = run_dir / f"payload-{server}-{label}.json"
            data = load_json(path)
            if data and "throughput_msg_per_sec" in data:
                tp = float(data["throughput_msg_per_sec"])
                msg_rates.append(tp)
                mbps_rates.append(tp * size / 1048576)
                valid_labels.append(label)

        if msg_rates:
            color = COLORS.get(server, "#666")
            ax1.plot(valid_labels, msg_rates, "o-", color=color,
                     label=server, linewidth=2, markersize=5)
            ax2.plot(valid_labels, mbps_rates, "o-", color=color,
                     label=server, linewidth=2, markersize=5)

    ax1.set_xlabel("Payload Size")
    ax1.set_ylabel("Throughput (msg/s)")
    ax1.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f"{x/1000:.0f}k"))
    ax1.set_title("Messages per Second")
    ax1.legend(loc="best", fontsize=8)

    ax2.set_xlabel("Payload Size")
    ax2.set_ylabel("Throughput (MB/s)")
    ax2.set_title("Data Throughput")
    ax2.legend(loc="best", fontsize=8)

    fig.suptitle("Payload Size Impact (1000 connections)", fontsize=13)
    fig.tight_layout()
    fig.savefig(out_dir / "payload.png", dpi=150)
    plt.close(fig)
    print("  payload.png")


def main():
    setup_style()
    run_dir = find_results_dir()
    out_dir = run_dir / "charts"
    out_dir.mkdir(exist_ok=True)

    print(f"[report] results from: {run_dir}")
    print(f"[report] generating charts...")

    plot_throughput_scaling(run_dir, out_dir)
    plot_latency(run_dir, out_dir)
    plot_connrate(run_dir, out_dir)
    plot_memory(run_dir, out_dir)
    plot_payload(run_dir, out_dir)

    print(f"\n[report] charts saved to {out_dir}/")


if __name__ == "__main__":
    main()
