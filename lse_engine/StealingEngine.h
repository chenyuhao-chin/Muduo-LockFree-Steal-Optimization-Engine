#pragma once
#include "WorkStealingDeque.h"
#include "PaddedAtomic.h"
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <functional>
#include <chrono>
#include <mutex>
#include <queue>

namespace lse {

// Work-Stealing 调度引擎
// ┌─────────────────────────────────────────────────────────────────┐
// │  外部线程（IO/Acceptor）                                         │
// │    submit(task) → inbox[i]  (mutex-protected MPSC inbox)        │
// │                                                                  │
// │  Worker 线程 i                                                   │
// │    1. drain inbox[i] → deque[i].push()   ← 唯一调用 push() 的地方│
// │    2. deque[i].pop()   (owner 快路径)                            │
// │    3. deque[j].steal() (随机偷邻居)                              │
// └─────────────────────────────────────────────────────────────────┘
// 这样 push()/pop() 始终由同一个 owner 线程调用，满足 Chase-Lev 契约。
// inbox 的 mutex 只在 drain 时持有，是冷路径，不影响 steal 热路径。
class StealingEngine {
public:
    explicit StealingEngine(int num_threads) : running_(false) {
        // Worker 含 mutex，不可拷贝/移动，用 unique_ptr 包装后放入 vector
        for (int i = 0; i < num_threads; ++i) {
            workers_.push_back(std::make_unique<Worker>());
            workers_.back()->queue = std::make_unique<WorkStealingDeque>();
        }
    }

    ~StealingEngine() { stop(); }

    void start() {
        running_.store(true, std::memory_order_relaxed);
        for (int i = 0; i < static_cast<int>(workers_.size()); ++i) {
            workers_[i]->thread = std::make_unique<std::thread>(
                &StealingEngine::worker_loop, this, i);
        }
    }

    void stop() {
        running_.store(false, std::memory_order_relaxed);
        for (auto& w : workers_) {
            if (w->thread && w->thread->joinable()) {
                w->thread->join();
            }
        }
    }

    // 外部线程安全提交：写入目标 worker 的 inbox，不碰 Chase-Lev deque
    void submit(Task task) {
        size_t idx = next_worker_.fetch_add(1, std::memory_order_relaxed)
                     % workers_.size();
        {
            std::lock_guard<std::mutex> lk(workers_[idx]->inbox_mu);
            workers_[idx]->inbox.push(std::move(task));
        }
    }

    int num_workers() const { return static_cast<int>(workers_.size()); }

private:
    // 把 inbox 里的任务 drain 进 Chase-Lev deque（仅 owner 线程调用）
    void drain_inbox(int my_id) {
        auto& w = *workers_[my_id];
        if (w.inbox.empty()) return;
        std::lock_guard<std::mutex> lk(w.inbox_mu);
        while (!w.inbox.empty()) {
            w.queue->push(std::move(w.inbox.front()));
            w.inbox.pop();
        }
    }

    void worker_loop(int my_id) {
        std::mt19937 rng(std::random_device{}() ^ static_cast<uint32_t>(my_id));
        std::uniform_int_distribution<int> dist(0, static_cast<int>(workers_.size()) - 1);
        int idle_spins = 0;

        while (running_.load(std::memory_order_relaxed)) {
            // 0. 把外部提交的任务搬进本地 deque（owner 线程独占 push）
            drain_inbox(my_id);

            // 1. 优先消费本地队列（无竞争，最快路径）
            if (auto task = workers_[my_id]->queue->pop()) {
                (*task)();
                idle_spins = 0;
                continue;
            }

            // 2. 本地空了，随机选一个受害者偷任务
            int victim = dist(rng);
            if (victim != my_id) {
                if (auto task = workers_[victim]->queue->steal()) {
                    (*task)();
                    idle_spins = 0;
                    continue;
                }
            }

            // 3. 没有任务：退避，避免空转与 IO 线程争核
            ++idle_spins;
            if (idle_spins < 64) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }

    struct Worker {
        std::unique_ptr<std::thread>        thread;
        std::unique_ptr<WorkStealingDeque>  queue;
        // MPSC inbox：外部线程写，owner 线程读后 drain 进 queue
        std::mutex          inbox_mu;
        std::queue<Task>    inbox;
    };

    std::vector<std::unique_ptr<Worker>>  workers_;
    std::atomic<bool>                     running_;
    PaddedAtomic<size_t>                  next_worker_{0};
};

} // namespace lse
