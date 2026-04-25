import subprocess
import time
import os
import re
import csv

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SERVER_BIN = os.path.join(ROOT_DIR, "echo_baseline")
DATA_DIR = os.path.join(ROOT_DIR, "benchmark_data")
ARCH_DIR = os.path.join(ROOT_DIR, "architecture")
TARGET_URL = "http://127.0.0.1:8500/"
THREAD_COUNTS = [1, 2, 4, 8, 16, 32]
WRK_THREADS = 4
WRK_CONNS = 500
WRK_DURATION = 30


def compile_server():
    # 修改 main.cc 使其支持从命令行参数读取线程数，然后编译 echo_baseline
    main_cc = os.path.join(ROOT_DIR, "examples/echo/main.cc")
    patched = '#include "echo.hpp"\n#include <cstdlib>\nint main(int argc, char* argv[]) {\n    int t = argc > 1 ? std::atoi(argv[1]) : 1;\n    EchoServer server(8500, t);\n    server.Start();\n    return 0;\n}\n'
    with open(main_cc, "w") as f:
        f.write(patched)

    cmd = [
        "g++", "-O3", "-std=c++17",
        "examples/echo/main.cc",
        "-I./muduo_net", "-I./examples/echo",
        "-o", "echo_baseline", "-lpthread"
    ]
    r = subprocess.run(cmd, cwd=ROOT_DIR, capture_output=True, text=True)
    if r.returncode != 0:
        print("Compile failed:", r.stderr)
        raise SystemExit(1)
    print("Compiled echo_baseline with argv thread support.")


def start_server(thread_num):
    # 以指定线程数启动 echo 服务器，等待 2 秒让其完成监听初始化
    proc = subprocess.Popen(
        [SERVER_BIN, str(thread_num)],
        cwd=ROOT_DIR,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    time.sleep(2)
    return proc


def get_sy_percent(pid, duration=WRK_DURATION):
    # 在 wrk 压测期间并行采样进程的内核态 CPU 占比（sy%），通过 /proc/<pid>/stat 获取精确数据
    samples = []
    deadline = time.time() + duration
    while time.time() < deadline:
        try:
            r = subprocess.run(
                ["top", "-b", "-n", "1", "-p", str(pid)],
                capture_output=True, text=True, timeout=5
            )
            for line in r.stdout.splitlines():
                parts = line.split()
                if parts and parts[0] == str(pid):
                    break
            # 从 /proc/<pid>/stat 读取精确的用户态/内核态 tick 数
            with open(f"/proc/{pid}/stat") as f:
                stat = f.read().split()
            # stat 字段：utime=索引13，stime=索引14（0-indexed）
            utime, stime = int(stat[13]), int(stat[14])
            samples.append((utime, stime, time.time()))  # 记录一次采样点
        except Exception:
            pass
        time.sleep(2)  # 每 2 秒采样一次，避免过度占用 CPU
    return samples


def compute_sy_percent(samples):
    # 根据首尾两个采样点计算整个压测期间的内核态 CPU 占比（sy%）
    if len(samples) < 2:
        return None
    t0, t1 = samples[0], samples[-1]
    elapsed_ticks = (t1[2] - t0[2]) * os.sysconf("SC_CLK_TCK")  # 墙钟时间换算成 CPU tick 数
    total_ticks = (t1[0] + t1[1]) - (t0[0] + t0[1])
    sy_ticks = t1[1] - t0[1]  # 仅统计内核态（stime）的增量
    if total_ticks <= 0:
        return None
    return round(sy_ticks / elapsed_ticks * 100, 2)


def run_wrk_with_sy(proc):
    # 并发执行 wrk 压测和 sy% 采样：后台线程持续采样，主线程跑 wrk，结束后汇总
    import threading

    pid = proc.pid
    sy_samples = []

    def sample():
        sy_samples.extend(get_sy_percent(pid, WRK_DURATION + 5))

    t = threading.Thread(target=sample, daemon=True)
    t.start()

    wrk_cmd = ["wrk", f"-t{WRK_THREADS}", f"-c{WRK_CONNS}", f"-d{WRK_DURATION}s", TARGET_URL]
    print(f"  wrk: {' '.join(wrk_cmd)}")
    r = subprocess.run(wrk_cmd, capture_output=True, text=True)
    t.join(timeout=10)

    return r.stdout, sy_samples


def parse_wrk(output):
    # 从 wrk 输出中解析 QPS 和最大延迟（统一换算为 ms）
    qps = lat_max = None
    m = re.search(r"Requests/sec:\s+([\d.]+)", output)
    if m:
        qps = float(m.group(1))
    # 匹配 Latency 行：avg  stdev  max，取第三列（max）
    m = re.search(r"Latency\s+[\d.]+\w+\s+[\d.]+\w+\s+([\d.]+)(ms|s|us)", output)
    if m:
        lat_max = float(m.group(1))
        unit = m.group(2)
        if unit == "s":
            lat_max *= 1000   # 秒转毫秒
        elif unit == "us":
            lat_max /= 1000   # 微秒转毫秒
    return qps, lat_max


def kill_server(proc):
    # 强制终止服务器进程，等待 2 秒确保端口完全释放
    proc.kill()
    proc.wait()
    time.sleep(2)


def plot_results(rows):
    # 将各线程数下的 QPS、最大延迟、sy% 绘制成折线图并保存到 benchmark_data/
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available, skipping plots.")
        return

    threads = [r[0] for r in rows]
    qps     = [r[1] for r in rows]
    lat_max = [r[2] for r in rows]
    sy      = [r[3] for r in rows]

    fig, ax = plt.subplots()
    ax.plot(threads, qps, marker="o")
    ax.set_xlabel("Thread Count")
    ax.set_ylabel("Requests/sec (QPS)")
    ax.set_title("Baseline QPS vs Thread Count")
    ax.set_xscale("log", base=2)
    fig.savefig(os.path.join(ARCH_DIR, "qps_vs_threads.png"), dpi=150)
    plt.close(fig)

    fig, ax1 = plt.subplots()
    ax1.set_xlabel("Thread Count")
    ax1.set_ylabel("Max Latency (ms)", color="tab:blue")
    ax1.plot(threads, lat_max, marker="o", color="tab:blue", label="Max Latency")
    ax1.tick_params(axis="y", labelcolor="tab:blue")
    ax1.set_xscale("log", base=2)

    ax2 = ax1.twinx()
    ax2.set_ylabel("sy% (kernel CPU)", color="tab:red")
    ax2.plot(threads, sy, marker="s", color="tab:red", linestyle="--", label="sy%")
    ax2.tick_params(axis="y", labelcolor="tab:red")

    fig.suptitle("Baseline Max Latency & sy% vs Thread Count")
    fig.tight_layout()
    fig.savefig(os.path.join(ARCH_DIR, "latency_sy_vs_threads.png"), dpi=150)
    plt.close(fig)

    print(f"Plots saved to {ARCH_DIR}/")


def main():
    # 主流程：依次用不同线程数启动服务器、跑 wrk 压测、采集 sy%，结果写入 CSV 并绘图
    os.makedirs(DATA_DIR, exist_ok=True)
    compile_server()

    csv_path = os.path.join(DATA_DIR, "threads_test.csv")
    rows = []

    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["threads", "qps", "max_latency_ms", "sy_percent"])

        for n in THREAD_COUNTS:
            print(f"\n--- threads={n} ---")
            proc = start_server(n)
            try:
                wrk_out, sy_samples = run_wrk_with_sy(proc)
            finally:
                kill_server(proc)

            qps, lat_max = parse_wrk(wrk_out)
            sy = compute_sy_percent(sy_samples)
            print(f"  QPS={qps}  MaxLat={lat_max}ms  sy%={sy}")
            writer.writerow([n, qps, lat_max, sy])
            rows.append((n, qps, lat_max, sy))

    print(f"\nData saved to {csv_path}")
    plot_results(rows)


if __name__ == "__main__":
    main()
