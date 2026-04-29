#pragma once
#include <cstdint>
#include <vector>
#include <chrono>

// CPU-bound 重计算任务（"西瓜任务"）。
// cpu_work(target_us) 运行约 target_us 微秒的纯 CPU 密集型计算。
//
// 原理：
//   1. thread_local 4096 int64 数组 + 质数步长访问 → 无法 cache prefetch
//   2. 首次调用时做**两阶段校准**：
//      a. 先跑 1 轮完整 4096 次 cache-missing 迭代，测量耗时 us_per_round
//      b. rounds = target_us / us_per_round
//   3. 无系统调用 (no malloc after init, no sleep, no IO)
//   4. volatile sink 防 DCE
//
inline void cpu_work(int64_t target_us) {
    static thread_local std::vector<int64_t> buf;
    static thread_local double us_per_round = 0;  // 每轮(4096次迭代)耗时微秒
    static thread_local bool ready = false;

    constexpr int64_t N = 4096;
    constexpr int64_t STEP = 1753;  // 与 4096 互质的质数步长

    if (!ready) {
        buf.resize(N, 0);
        for (size_t i = 0; i < N; i++) {
            buf[i] = static_cast<int64_t>((i * 1103515245LL + 12345) & 0x7fffffff);
        }

        // 阶段一：跑 CALIB_ROUNDS 轮，测量总耗时
        constexpr int64_t CALIB_ROUNDS = 100;  
        volatile int64_t sink = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int64_t r = 0; r < CALIB_ROUNDS; r++) {
            for (size_t j = 0, i = 0; j < N; j++, i = (i + STEP) % N) {
                buf[i] = buf[i] * 6364136223846793005LL + 1442695040888963407LL;
                sink ^= buf[i];
            }
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double total_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (total_us < 1) total_us = 1;
        us_per_round = total_us / CALIB_ROUNDS;  // 每轮平均耗时(μs)

        ready = true;
    }

    // 线性计算需要多少轮
    int64_t rounds = static_cast<int64_t>(static_cast<double>(target_us) / us_per_round + 0.5);
    if (rounds < 1) rounds = 1;

    volatile int64_t sink = 0;
    for (int64_t r = 0; r < rounds; r++) {
        for (size_t j = 0, i = 0; j < N; j++, i = (i + STEP) % N) {
            buf[i] = buf[i] * 6364136223846793005LL + 1442695040888963407LL;
            sink ^= buf[i];
        }
    }
    (void)sink;
}
