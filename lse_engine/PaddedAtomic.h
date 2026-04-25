#pragma once
#include <atomic>
#include <new>

namespace lse {

// 缓存行大小：C++17 标准提供，通常是 64 字节
// 如果编译器不支持，回退到 64
constexpr std::size_t CACHE_LINE_SIZE =
#ifdef __cpp_lib_hardware_interference_size
    std::hardware_destructive_interference_size;
#else
    64;
#endif

// 将原子变量对齐到一整个缓存行，防止伪共享（False Sharing）
// 伪共享：两个无关变量落在同一缓存行，一个被修改会导致另一个的缓存失效
// 代价：每个 PaddedAtomic 至少占 64 字节，用空间换性能
template <typename T>
struct alignas(CACHE_LINE_SIZE) PaddedAtomic {
    std::atomic<T> value;

    PaddedAtomic(T initial = T{}) noexcept : value(initial) {}

    PaddedAtomic(const PaddedAtomic&) = delete;
    PaddedAtomic& operator=(const PaddedAtomic&) = delete;

    T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return value.load(order);
    }
    void store(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
        value.store(desired, order);
    }
    T fetch_add(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value.fetch_add(arg, order);
    }
    bool compare_exchange_strong(T& expected, T desired,
                                  std::memory_order success,
                                  std::memory_order failure) noexcept {
        return value.compare_exchange_strong(expected, desired, success, failure);
    }
};

} // namespace lse
