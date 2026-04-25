# Muduo-LockFree-Steal-Optimization-Engine

这是一个基于 Muduo 网络库构建的性能导向型任务调度引擎。

本项目将 Muduo 默认的全局互斥锁线程池替换为无锁的任务窃取（Work-Stealing）调度器，旨在消除非均衡负载（如 LLM 推理、高计算量任务）场景下的长尾延迟与锁竞争。

## 架构设计

系统实现了网络 I/O 与计算任务的深度解耦：
* **网络 I/O 层**：保留 Muduo 基于 `epoll` 的 Reactor 模型（One Loop Per Thread），用于处理高并发连接。
* **任务调度层**：引入去中心化的无锁任务窃取引擎，专职处理 CPU 密集型任务。

## 核心优化点

* **无锁任务窃取 (Chase-Lev Deque)**
    每个 Worker 线程维护一个私有的双端队列。线程内部通过 LIFO（后进先出）模式处理本地任务，以最大化 CPU 缓存局部性（Cache Locality）。当核心空闲时，通过 FIFO（先进先出）模式从其他队列窃取任务。核心逻辑采用 `CAS (Compare-And-Swap)` 操作与严格的内存屏障（`acquire/release` 语义），彻底消除全局锁瓶颈。
* **伪共享消除 (False Sharing Elimination)**
    针对高频竞争的关键原子变量（如队列的 `top` 与 `bottom` 指针），封装 `PaddedAtomic` 类并使用 `alignas(64)` 进行缓存行填充。此举强制关键指针位于独立的 Cache Line 中，防止 MESI 协议触发缓存失效风暴，显著降低跨核同步开销。

## 目录结构

```text
├── muduo_net/          # Muduo 网络库核心 (EventLoop, TcpConnection, Poller)
├── lse_engine/         # 无锁调度引擎核心
│   ├── PaddedAtomic.h        # 缓存行对齐的原子封装类
│   ├── WorkStealingDeque.h   # Chase-Lev 队列实现
│   └── StealingEngine.h      # Worker 线程与任务管理
├── examples/           # 性能基准测试与模拟示例
└── CMakeLists.txt
```

## 构建与运行

**环境要求：**
* Linux Kernel 5.0+
* CMake 3.10+
* GCC/Clang with C++14 support

```bash
# 1. 克隆仓库
git clone https://github.com/chenyuhao-chin/Muduo-LockFree-Steal-Optimization-Engine.git
cd Muduo-LockFree-Steal-Optimization-Engine

# 2. 开启 Release 性能优化编译
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# 3. 运行性能基准测试 (开发中)
./bin/engine_bench
