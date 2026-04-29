#!/usr/bin/env python3
"""
混合负载（Mixed-Load）对比基准测试。

测试类型:
  mode=hello   → /hello 端点（纯轻量，类似原始 baseline_vs_lse_http.csv 测试）
  mode=mixed   → /mixed 端点（heavy_pct% 概率执行 ~50ms CPU 密集计算）

关键输出:
  1. CSV 数据文件到 benchmark_data/
  2. QPS 对比图（原始）
  3. P50 / P99 / P999 延迟分位数对比图（混合负载亮点）
"""
import subprocess, time, os, re, csv, threading, sys

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BIN     = os.path.join(ROOT, "benchmark", "http_server")
DATA    = os.path.join(ROOT, "benchmark_data")
ARCH    = os.path.join(ROOT, "architecture")

# wrk 参数
WRK_T, WRK_C, WRK_D = 4, 100, 30

# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------
def build():
    r = subprocess.run(["make", "-C", os.path.join(ROOT, "benchmark"), "-B"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("Build failed:\n", r.stderr); sys.exit(1)
    print("Build OK.")


def start(io, lse, heavy_pct=0):
    """
    启动 http_server。
    heavy_pct > 0 时添加 --heavy-pct 参数（启用 /mixed 端点）。
    """
    args = [BIN, "--port", "8095", "--io", str(io)]
    if lse:
        args += ["--lse", str(lse)]
    if heavy_pct > 0:
        args += ["--heavy-pct", str(heavy_pct)]
    p = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(1.5)  # 等待服务器就绪
    return p


def kill(p):
    p.kill(); p.wait(); time.sleep(1.5)


def sy_sample(duration):
    """
    基于全局 /proc/stat 的 CPU 行测量 system-time 占比。
    读取两次 /proc/stat 的 cpu 行，delta 期间 system time 的百分比。
    完全避开 /proc/{pid}/stat 的字段解析问题。
    """
    try:
        with open("/proc/stat") as f:
            parts = f.readline().split()
        # cpu  user nice system idle iowait irq softirq steal guest guest_nice
        # cpu  123  456  789    ...
        # user_nice = parts[2] (old kernels) or parts[3] (new)
        # 安全做法：解析已知字段
        if len(parts) < 5:
            return None
        u0, n0, s0 = int(parts[1]), int(parts[2]), int(parts[3])
        t0 = time.monotonic()
        time.sleep(duration)
        with open("/proc/stat") as f:
            parts2 = f.readline().split()
        if len(parts2) < 5:
            return None
        u1, n1, s1 = int(parts2[1]), int(parts2[2]), int(parts2[3])
        elapsed = time.monotonic() - t0
        if elapsed <= 0:
            return None
        total_delta = (u1 + n1 + s1) - (u0 + n0 + s0)
        sys_delta = s1 - s0
        if total_delta <= 0:
            return None
        return round(sys_delta / total_delta * 100, 2)
    except Exception:
        return None


def run_wrk(p, url, duration=WRK_D):
    r = subprocess.run(
        ["wrk", f"-t{WRK_T}", f"-c{WRK_C}", f"-d{duration}s",
         "--latency", url],
        capture_output=True, text=True, timeout=duration + 10)
    sy = sy_sample(duration + 4)
    return r.stdout, sy


def parse_legacy(out):
    """从 wrk 输出解析 QPS 和 Max Latency（兼容原始格式）。"""
    qps = lat = None
    m = re.search(r"Requests/sec:\s+([\d.]+)", out)
    if m: qps = float(m.group(1))
    m = re.search(r"Latency\s+[\d.]+\w+\s+[\d.]+\w+\s+([\d.]+)(ms|s|us)", out)
    if m:
        lat = float(m.group(1))
        if m.group(2) == "s": lat *= 1000
        elif m.group(2) == "us": lat /= 1000
    return qps, lat


def parse_latency_pcts(out):
    """
    从 wrk --latency 输出解析分位数延迟。

    预期格式（在详细统计信息后）:

      Latency Distribution (HdrHistogram - Recorded Latency)
     50.000%    1.38ms
     75.000%    1.91ms
     90.000%    2.08ms
     99.000%    2.08ms
     99.900%    2.14ms
     99.990%    2.14ms

    返回 {p50_ms:float, p75_ms:float, p90_ms:float, p99_ms:float, p999_ms:float}
    """
    pcts = {}
    lines = out.splitlines()
    in_dist = False
    pattern = re.compile(
        r"^\s*(?P<pct>[\d.]+)%\s+(?P<val>[\d.]+)(?P<unit>ms|s|us)\s*$"
    )
    for line in lines:
        if "Latency Distribution" in line:
            in_dist = True
            continue
        if in_dist:
            m = pattern.match(line)
            if m:
                pct_str = m.group("pct")
                val = float(m.group("val"))
                unit = m.group("unit")
                if unit == "s":
                    val *= 1000
                elif unit == "us":
                    val /= 1000
                # 转换为 ms
                pcts[pct_str] = val
    # 规范化到标准键名（兼容 "50" 和 "50.000" 两种格式）
    result = {
        "p50_ms":  pcts.get("50") or pcts.get("50.000", None),
        "p75_ms":  pcts.get("75") or pcts.get("75.000", None),
        "p90_ms":  pcts.get("90") or pcts.get("90.000", None),
        "p99_ms":  pcts.get("99") or pcts.get("99.000", None),
        "p999_ms": pcts.get("99.9") or pcts.get("99.900", None),
    }
    return result


# ---------------------------------------------------------------------------
# 测量
# ---------------------------------------------------------------------------
def measure(io, lse, heavy_pct=0, duration=WRK_D):
    """运行一次测试，返回 (qps, max_lat, sy_pct, pcts_dict)。"""
    label = "LSE" if lse else "Baseline"
    print(f"  [{label}] io={io} lse={lse} heavy={heavy_pct}% "
          f"dur={duration}s ...", end=" ", flush=True)
    p = start(io, lse, heavy_pct)
    try:
        if heavy_pct > 0:
            url = f"http://127.0.0.1:8095/mixed"
        else:
            url = f"http://127.0.0.1:8095/hello"
        out, sy = run_wrk(p, url, duration)
    finally:
        kill(p)

    qps, max_lat = parse_legacy(out)
    pcts = parse_latency_pcts(out)

    def _fmt(v):
        return f"{v:>8.2f}" if v is not None else f"{'?':>8}"
    print(f"QPS={qps:.0f}  P50={_fmt(pcts.get('p50_ms'))}ms  "
          f"P99={_fmt(pcts.get('p99_ms'))}ms  "
          f"P999={_fmt(pcts.get('p999_ms'))}ms  sy%={sy}")
    return qps, max_lat, sy, pcts


# ---------------------------------------------------------------------------
# 绘图
# ---------------------------------------------------------------------------
def plot_hello(base, lse_rows):
    """原始轻量 /hello 对比图（QPS + Max Latency + sy%）。"""
    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available, skipping plot"); return

    os.makedirs(ARCH, exist_ok=True)
    threads = [r[0] for r in base]  # io_threads
    lbl = ["baseline", "lse"]

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(threads, [r[1] for r in base], "o-", label="Baseline (mutex)")
    ax.plot(threads, [r[1] for r in lse_rows], "s--", label="LSE (work-stealing)")
    ax.set_xlabel("IO Threads"); ax.set_ylabel("Requests/sec")
    ax.set_title("QPS: Baseline vs LSE (/hello)"); ax.legend(); ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(ARCH, "qps_vs_threads.png"), dpi=150)
    plt.close(fig)
    print(f"  → {ARCH}/qps_vs_threads.png")


def plot_mixed(baseline_data, lse_data):
    """
    混合负载对比图：P50 / P99 / P999 延迟分位数。
    baseline_data / lse_data 结构: [(io, qps, max_lat, sy, pcts_dict), ...]
    
    生成三张子图的大图。
    """
    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available, skipping mixed plot"); return

    os.makedirs(ARCH, exist_ok=True)

    threads = [r[0] for r in baseline_data]
    # 提取各分位数
    pct_keys = ["p50_ms", "p99_ms", "p999_ms"]
    pct_labels = ["P50 Latency (ms)", "P99 Latency (ms)", "P999 Latency (ms)"]
    pct_titles = ["P50 Latency: Baseline vs LSE (mixed-load)",
                  "P99 Latency: Baseline vs LSE (mixed-load)",
                  "P999 Latency: Baseline vs LSE (mixed-load)"]

    fig, axes = plt.subplots(1, 3, figsize=(15, 5))

    for idx, key, label, title in zip(range(3), pct_keys, pct_labels, pct_titles):
        ax = axes[idx]
        b_vals = [r[4].get(key, None) for r in baseline_data]
        l_vals = [r[4].get(key, None) for r in lse_data]
        ax.plot(threads, b_vals, "o-", color="tab:blue", label="Baseline")
        ax.plot(threads, l_vals, "s--", color="tab:orange", label="LSE")
        ax.set_xlabel("IO Threads")
        ax.set_ylabel(label)
        ax.set_title(title, fontsize=10)
        ax.legend(); ax.grid(alpha=0.3)

    fig.suptitle("Mixed-Load Latency Percentiles: Baseline vs LSE (work-stealing)",
                 fontsize=12, y=1.02)
    fig.tight_layout()
    fig.savefig(os.path.join(ARCH, "mixed_latency_pcts.png"), dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  → {ARCH}/mixed_latency_pcts.png")


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="Mixed-Load Benchmark: Baseline vs LSE")
    parser.add_argument("--mode", choices=["hello", "mixed"], default="mixed",
                        help="测试负载类型 (default: mixed)")
    parser.add_argument("--heavy-pct", type=int, default=20,
                        help="混合负载中重计算任务的概率百分比 (default: 20)")
    parser.add_argument("--io", type=int, nargs="+", default=[1, 2, 4, 8],
                        help="要测试的 IO 线程数列表 (default: 1 2 4 8)")
    parser.add_argument("--duration", type=int, default=30,
                        help="每次 wrk 测试时长（秒）(default: 30)")
    parser.add_argument("--no-plot", action="store_true",
                        help="不生成图表")
    args = parser.parse_args()

    os.makedirs(DATA, exist_ok=True)

    # 清理残留进程
    subprocess.run(["pkill", "-f", "http_server"], capture_output=True)
    time.sleep(1)

    # 编译
    build()

    mode_name = args.mode
    heavy_pct = args.heavy_pct if args.mode == "mixed" else 0
    io_list = args.io
    duration = args.duration

    # CSV 文件名
    if mode_name == "hello":
        csv_name = "baseline_vs_lse_http.csv"
        csv_cols = ["mode", "io_threads", "lse_workers", "qps",
                    "max_latency_ms", "sy_percent"]
    else:
        csv_name = f"mixed_load_h{heavy_pct}.csv"
        csv_cols = ["mode", "io_threads", "lse_workers", "qps",
                    "max_latency_ms", "sy_percent",
                    "p50_ms", "p75_ms", "p90_ms", "p99_ms", "p999_ms"]

    csv_path = os.path.join(DATA, csv_name)
    base_rows = []  # [(io, qps, max_lat, sy, pcts)]
    lse_rows = []   # [(io, qps, max_lat, sy, pcts)]

    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(csv_cols)

        for io in io_list:
            print(f"\n{'='*50}")
            print(f"IO Threads = {io}")
            print(f"{'='*50}")

            # Baseline (no LSE)
            qps, lat, sy, pcts = measure(io, lse=0,
                                         heavy_pct=heavy_pct, duration=duration)
            base_rows.append((io, qps, lat, sy, pcts))
            row = ["baseline", io, 0, qps, lat, sy]
            if mode_name == "mixed":
                row += [pcts.get("p50_ms", ""), pcts.get("p75_ms", ""),
                        pcts.get("p90_ms", ""), pcts.get("p99_ms", ""),
                        pcts.get("p999_ms", "")]
            w.writerow(row)

            # LSE (workers = io)
            qps, lat, sy, pcts = measure(io, lse=io,
                                         heavy_pct=heavy_pct, duration=duration)
            lse_rows.append((io, qps, lat, sy, pcts))
            row = ["lse", io, io, qps, lat, sy]
            if mode_name == "mixed":
                row += [pcts.get("p50_ms", ""), pcts.get("p75_ms", ""),
                        pcts.get("p90_ms", ""), pcts.get("p99_ms", ""),
                        pcts.get("p999_ms", "")]
            w.writerow(row)

    print(f"\nData saved → {csv_path}")

    # 绘图
    if not args.no_plot:
        if mode_name == "hello":
            plot_hello(base_rows, lse_rows)
        else:
            plot_mixed(base_rows, lse_rows)

    print("Done.")


if __name__ == "__main__":
    main()
