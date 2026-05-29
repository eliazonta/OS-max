#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include "types.hpp"

/**
 * Lock-free Single-Producer Single-Consumer ring buffer.
 * Relies on strict acquire/release memory semantics to avoid locks/mutexes.
 */
template <typename T, uint32_t N>
    requires(N > 0 && (N & (N - 1)) == 0) // N must be a power of 2 for fast masking
class alignas(CACHE_LINE) SPSCQueue
{
public:
    bool push(const T &item) noexcept
    {
        uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= N)
            return false;
        buf_[head & MASK] = item;
        // Release: makes the write visible to the consumer thread
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(T &out) noexcept
    {
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire: sees the producer's release
        if (tail == head_.load(std::memory_order_acquire))
            return false;

        out = buf_[tail & MASK];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    uint32_t size() const noexcept
    {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr uint32_t MASK = N - 1;
    std::array<T, N> buf_{};

    // Pad atomic counters to separate cache lines so they never falsely share
    alignas(CACHE_LINE) std::atomic<uint32_t> head_{0};
    alignas(CACHE_LINE) std::atomic<uint32_t> tail_{0};
};