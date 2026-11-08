#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace shirane::audio {
#ifdef _MSC_VER
#pragma warning(push)
// Padding is intentional: producer/consumer indices must not share a cache line.
#pragma warning(disable: 4324)
#endif
// Exactly one producer and one consumer. Construct/destroy only while both are stopped.
// T must contain its payload or use an independently defined ownership protocol.
template<class T, std::size_t Capacity>
class SpscQueue final {
    static_assert(Capacity > 0 && Capacity < std::numeric_limits<std::size_t>::max());
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_copy_assignable_v<T>);
    static_assert(std::is_nothrow_default_constructible_v<T>);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
public:
    SpscQueue() noexcept = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    [[nodiscard]] bool tryPush(const T& value) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = advance(write);
        if (next == read_.load(std::memory_order_acquire)) return false;
        slots_[write] = value;
        write_.store(next, std::memory_order_release);
        return true;
    }
    [[nodiscard]] bool tryPop(T& value) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        value = slots_[read];
        read_.store(advance(read), std::memory_order_release);
        return true;
    }
    static constexpr std::size_t capacity() noexcept { return Capacity; }
private:
    static constexpr std::size_t advance(std::size_t index) noexcept {
        return index == Capacity ? 0 : index + 1;
    }
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::array<T, Capacity + 1> slots_{};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}
