#pragma once
#include "PaddedAtomic.h"
#include <vector>
#include <optional>
#include <functional>
#include <cassert>

namespace lse {

using Task = std::function<void()>;

// Chase-Lev 无锁工作窃取双端队列
// 所有者（Owner）从 bottom 端 push/pop（无竞争，无锁）
// 窃贼（Thief）从 top 端 steal（有竞争，用 CAS）
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(size_t capacity = 1024) {
        // 容量必须是 2 的幂，方便用位运算取模
        assert((capacity & (capacity - 1)) == 0);
        buffer_.resize(capacity);
        mask_ = capacity - 1;
    }

    // 仅 Owner 线程调用：把任务压入底部
    void push(Task task) {
        // relaxed：只有 Owner 写 bottom，无需与其他线程同步
        int64_t b = bottom_.load(std::memory_order_relaxed);
        // acquire：读 top 需要看到 steal() 对 top 的最新写入，防止误判队列未满
        int64_t t = top_.load(std::memory_order_acquire);

        // 队列满了就扩容（2 倍）
        if (b - t >= static_cast<int64_t>(buffer_.size())) {
            grow(b, t);
        }

        buffer_[b & mask_] = std::move(task);

        // release fence：保证 buffer_[b] 的写入在 bottom 自增之前对所有线程可见，
        // 防止 steal() 读到 bottom 已更新但 buffer_ 内容还未写入的中间状态
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    // 仅 Owner 线程调用：从底部弹出一个任务自己执行
    std::optional<Task> pop() {
        // relaxed：Owner 独占写 bottom，无需跨线程同步
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        // 先把 bottom 往回收一格，"预占"这个槽位
        bottom_.store(b, std::memory_order_relaxed);

        // seq_cst fence：在读 top 之前建立全序屏障，与 steal() 的 seq_cst CAS 配对，
        // 确保 Owner 和 Thief 对"最后一个任务"的争抢结果全局一致，不会双方都拿到
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // relaxed：seq_cst fence 已保证顺序，此处无需额外同步语义
        int64_t t = top_.load(std::memory_order_relaxed);

        if (t > b) {
            // 队列已空，恢复 bottom
            bottom_.store(b + 1, std::memory_order_relaxed);
            return std::nullopt;
        }

        if (t < b) {
            // 队列里还有多个任务，Owner 独占，直接返回，无需 CAS
            return std::move(buffer_[b & mask_]);
        }

        // 队列里只剩 1 个任务（t == b），Owner 和 Thief 可能同时来抢
        // seq_cst：与 steal() 的 seq_cst CAS 形成全序，保证只有一方成功
        // 失败时 relaxed 即可，因为失败说明已被抢走，无需同步任何数据
        bool won = top_.compare_exchange_strong(
            t, t + 1,
            std::memory_order_seq_cst,
            std::memory_order_relaxed
        );
        // 无论输赢，都把 bottom 恢复到空队列状态
        bottom_.store(b + 1, std::memory_order_relaxed);
        return won ? std::optional<Task>(std::move(buffer_[b & mask_])) : std::nullopt;
    }

    // 窃贼线程调用：从顶部偷走一个任务
    std::optional<Task> steal() {
        // acquire：读 top 后需要看到其他线程对 top 的最新修改
        int64_t t = top_.load(std::memory_order_acquire);
        // seq_cst fence：与 pop() 的 seq_cst fence 配对，建立全序，
        // 确保读 bottom 一定发生在读 top 之后，避免看到过期的空队列状态
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // acquire：确保读到 bottom 后，能看到 Owner push() 写入 buffer_ 的内容
        int64_t b = bottom_.load(std::memory_order_acquire);

        if (t >= b) {
            return std::nullopt; // 队列空
        }

        // seq_cst：与其他窃贼及 pop() 的 CAS 形成全序，保证只有一个窃贼成功推进 top
        // 失败时 relaxed 即可，放弃本次窃取无需同步任何数据
        if (!top_.compare_exchange_strong(
                t, t + 1,
                std::memory_order_seq_cst,
                std::memory_order_relaxed)) {
            return std::nullopt;
        }

        // CAS 成功后才 move，避免失败时留下 moved-from 对象
        return std::move(buffer_[t & mask_]);
    }

    size_t size() const {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return static_cast<size_t>(b > t ? b - t : 0);
    }

private:
    void grow(int64_t b, int64_t t) {
        size_t new_cap = buffer_.size() * 2;
        std::vector<Task> new_buf(new_cap);
        for (int64_t i = t; i < b; ++i) {
            new_buf[i & (new_cap - 1)] = std::move(buffer_[i & mask_]);
        }
        buffer_ = std::move(new_buf);
        mask_ = new_cap - 1;
    }

    std::vector<Task> buffer_;
    size_t mask_;

    // top 和 bottom 各占一个独立缓存行，彻底消灭伪共享
    PaddedAtomic<int64_t> top_{0};
    PaddedAtomic<int64_t> bottom_{0};
};

} // namespace lse
