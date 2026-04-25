#!/usr/bin/env python3
"""Baseline vs LSE benchmark — HTTP /hello endpoint, wrk -t4 -c500 -d30s"""
import subprocess, time, os, re, csv, threading, sys

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BIN     = os.path.join(ROOT, "benchmark", "http_server")
DATA    = os.path.join(ROOT, "benchmark_data")
ARCH    = os.path.join(ROOT, "architecture")
URL     = "http://127.0.0.1:8085/hello"

IO_COUNTS   = [1, 2, 4, 8]
WRK_T, WRK_C, WRK_S = 4, 500, 30


def build():
    r = subprocess.run(["make", "-C", os.path.join(ROOT, "benchmark"), "-B"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("Build failed:\n", r.stderr); sys.exit(1)
    print("Build OK.")


def start(io, lse):
    args = [BIN, "--port", "8085", "--io", str(io)]
    if lse: args += ["--lse", str(lse)]
    p = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    return p


def kill(p):
    p.kill(); p.wait(); time.sleep(1.5)


def sy_sample(pid, secs):
    def _stat():
        with open(f"/proc/{pid}/stat") as f: s = f.read().split()
        return int(s[13]), int(s[14])
    try:
        u0, s0 = _stat(); t0 = time.monotonic()
        time.sleep(secs)
        u1, s1 = _stat(); t1 = time.monotonic()
        ticks = (t1 - t0) * os.sysconf("SC_CLK_TCK")
        return round((s1 - s0) / ticks * 100, 2) if ticks > 0 else None
    except Exception:
        return None


def run_wrk(p):
    sy_res = [None]
    def _sy(): sy_res[0] = sy_sample(p.pid, WRK_S + 2)
    t = threading.Thread(target=_sy, daemon=True); t.start()
    r = subprocess.run(["wrk", f"-t{WRK_T}", f"-c{WRK_C}", f"-d{WRK_S}s", URL],
                       capture_output=True, text=True)
    t.join(timeout=15)
    return r.stdout, sy_res[0]


def parse(out):
    qps = lat = None
    m = re.search(r"Requests/sec:\s+([\d.]+)", out)
    if m: qps = float(m.group(1))
    m = re.search(r"Latency\s+[\d.]+\w+\s+[\d.]+\w+\s+([\d.]+)(ms|s|us)", out)
    if m:
        lat = float(m.group(1))
        if m.group(2) == "s": lat *= 1000
        elif m.group(2) == "us": lat /= 1000
    return qps, lat


def measure(io, lse):
    label = "LSE" if lse else "Baseline"
    print(f"  [{label}] io={io} lse={lse} ...", end=" ", flush=True)
    p = start(io, lse)
    try:
        out, sy = run_wrk(p)
    finally:
        kill(p)
    qps, lat = parse(out)
    print(f"QPS={qps:.0f}  MaxLat={lat:.1f}ms  sy%={sy}")
    return qps, lat, sy


def plot(base, lse_rows):
    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available"); return

    os.makedirs(ARCH, exist_ok=True)
    threads = IO_COUNTS
    def v(rows, i): return [r[i] if r[i] else 0 for r in rows]

    # 图1: QPS
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(threads, v(base, 0), "o-", label="Baseline (mutex)")
    ax.plot(threads, v(lse_rows, 0), "s--", label="LSE (work-stealing)")
    ax.set_xlabel("IO Threads"); ax.set_ylabel("Requests/sec")
    ax.set_title("QPS: Baseline vs LSE"); ax.legend(); ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(ARCH, "qps_vs_threads.png"), dpi=150)
    plt.close(fig)

    # 图2: Max Latency + sy% 双Y轴
    fig, ax1 = plt.subplots(figsize=(8, 5))
    ax1.set_xlabel("IO Threads")
    ax1.set_ylabel("Max Latency (ms)", color="tab:blue")
    ax1.plot(threads, v(base, 1), "o-", color="tab:blue", label="Baseline Latency")
    ax1.plot(threads, v(lse_rows, 1), "s--", color="steelblue", label="LSE Latency")
    ax1.tick_params(axis="y", labelcolor="tab:blue")
    ax2 = ax1.twinx()
    ax2.set_ylabel("sy% (kernel CPU)", color="tab:red")
    ax2.plot(threads, v(base, 2), "o-", color="tab:red", label="Baseline sy%")
    ax2.plot(threads, v(lse_rows, 2), "s--", color="salmon", label="LSE sy%")
    ax2.tick_params(axis="y", labelcolor="tab:red")
    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(lines1 + lines2, labels1 + labels2, loc="upper left")
    fig.suptitle("Max Latency & sy%: Baseline vs LSE"); fig.tight_layout()
    fig.savefig(os.path.join(ARCH, "latency_sy_vs_threads.png"), dpi=150)
    plt.close(fig)
    print(f"Plots → {ARCH}/")


def main():
    os.makedirs(DATA, exist_ok=True)
    build()
    base, lse_rows = [], []
    csv_path = os.path.join(DATA, "baseline_vs_lse_http.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["mode", "io_threads", "lse_workers", "qps", "max_latency_ms", "sy_percent"])
        for io in IO_COUNTS:
            print(f"\n=== io_threads={io} ===")
            qps, lat, sy = measure(io, lse=0)
            base.append((qps, lat, sy)); w.writerow(["baseline", io, 0, qps, lat, sy])
            qps, lat, sy = measure(io, lse=io)
            lse_rows.append((qps, lat, sy)); w.writerow(["lse", io, io, qps, lat, sy])
    print(f"\nData → {csv_path}")
    plot(base, lse_rows)


if __name__ == "__main__":
    main()
