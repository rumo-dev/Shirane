#include "audio/core/AudioBufferPool.h"
#include "audio/core/SpscQueue.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <thread>

namespace {
thread_local bool watching = false;
thread_local unsigned allocations = 0, releases = 0;
void* allocate(std::size_t size, std::size_t alignment = 0) {
    if (watching) ++allocations;
    void* p = alignment ? _aligned_malloc(size ? size : 1, alignment) : std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void deallocate(void* p, bool aligned = false) noexcept {
    if (watching && p) ++releases;
    if (aligned) _aligned_free(p); else std::free(p);
}
}
void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { deallocate(p); }
void operator delete[](void* p) noexcept { deallocate(p); }
void operator delete(void* p, std::size_t) noexcept { deallocate(p); }
void operator delete[](void* p, std::size_t) noexcept { deallocate(p); }
void operator delete(void* p, std::align_val_t) noexcept { deallocate(p, true); }
void operator delete[](void* p, std::align_val_t) noexcept { deallocate(p, true); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { deallocate(p, true); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { deallocate(p, true); }

namespace {
using namespace shirane::audio;
TEST(SpscQueueTest, FullEmptyWrapAndFailedOperationsPreserveData) {
    SpscQueue<unsigned, 3> queue;
    unsigned value = 99;
    EXPECT_FALSE(queue.tryPop(value)); EXPECT_EQ(value, 99u);
    for (unsigned round = 0; round < 10000; ++round) {
        for (unsigned i = 0; i < 3; ++i) ASSERT_TRUE(queue.tryPush(round * 3 + i));
        EXPECT_FALSE(queue.tryPush(999));
        for (unsigned i = 0; i < 3; ++i) {
            ASSERT_TRUE(queue.tryPop(value)); EXPECT_EQ(value, round * 3 + i);
        }
        EXPECT_FALSE(queue.tryPop(value));
    }
    SpscQueue<unsigned, 1> single;
    EXPECT_TRUE(single.tryPush(7)); EXPECT_FALSE(single.tryPush(8));
    EXPECT_TRUE(single.tryPop(value)); EXPECT_EQ(value, 7u);
}
TEST(SpscQueueTest, ConcurrentMillionPayloadsRemainOrdered) {
    struct Packet { std::uint64_t sequence = 0, inverse = 0; std::array<float, 8> samples{}; };
    SpscQueue<Packet, 127> queue;
    constexpr std::uint64_t kTransfers = 1'000'000;
    std::atomic<bool> stop{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kTransfers && !stop.load(); ++i) {
            Packet packet{i, ~i, {}}; packet.samples.fill(static_cast<float>(i));
            while (!queue.tryPush(packet)) { if (stop.load()) return; std::this_thread::yield(); }
        }
    });
    bool correct = true;
    std::uint64_t received = 0;
    while (received < kTransfers && std::chrono::steady_clock::now() < deadline) {
        Packet packet;
        if (!queue.tryPop(packet)) { std::this_thread::yield(); continue; }
        correct &= packet.sequence == received && packet.inverse == ~received;
        for (auto sample : packet.samples) correct &= sample == static_cast<float>(received);
        ++received;
    }
    stop.store(true); producer.join();
    EXPECT_EQ(received, kTransfers); EXPECT_TRUE(correct);
}
TEST(AudioBufferPoolTest, CapacityOwnershipAndStaleHandles) {
    AudioBufferPool<2, 256> pool, other;
    decltype(pool)::Handle first, second, failed;
    EXPECT_FALSE(pool.acquire(0, 1, failed)); EXPECT_FALSE(pool.acquire(257, 1, failed));
    EXPECT_FALSE(pool.acquire(1, 0, failed)); EXPECT_FALSE(pool.acquire(1, 3, failed));
    ASSERT_TRUE(pool.acquire(128, 2, first)); ASSERT_TRUE(pool.acquire(1, 1, second));
    EXPECT_EQ(pool.available(), 0u); EXPECT_FALSE(pool.acquire(1, 1, failed));
    EXPECT_EQ(failed.owner, nullptr); EXPECT_FALSE(other.release(first));
    auto view = pool.view(first); EXPECT_EQ(view.channelCount, 2u);
    EXPECT_NE(view.channels[0].data(), view.channels[1].data());
    view.channels[0][0] = 1; EXPECT_EQ(view.channels[1][0], 0);
    EXPECT_TRUE(pool.release(first)); EXPECT_FALSE(pool.release(first));
    ASSERT_TRUE(pool.acquire(256, 2, failed));
    EXPECT_EQ(pool.view(first).frameCount, 0u); EXPECT_FALSE(pool.release(first));
    EXPECT_EQ(pool.view(failed).channels[0][0], 0);
    EXPECT_TRUE(pool.release(second)); EXPECT_TRUE(pool.release(failed));
    EXPECT_EQ(pool.available(), 2u);
}
TEST(AudioBufferPoolTest, VariableFramesAndMonoStereoReuse) {
    AudioBufferPool<1, 256> pool;
    for (std::size_t frames = 1; frames <= 256; ++frames) for (std::size_t channels = 1; channels <= 2; ++channels) {
        decltype(pool)::Handle handle;
        ASSERT_TRUE(pool.acquire(frames, channels, handle));
        auto view = pool.view(handle);
        EXPECT_EQ(view.frameCount, frames); EXPECT_EQ(view.channelCount, channels);
        if (channels == 1) EXPECT_TRUE(view.channels[1].empty());
        for (std::size_t c = 0; c < channels; ++c) {
            ASSERT_EQ(view.channels[c].size(), frames);
            for (auto& sample : view.channels[c]) { EXPECT_EQ(sample, 0); sample = 0.75f; }
        }
        EXPECT_TRUE(pool.release(handle));
    }
}
TEST(AudioCoreRtTest, AllocationHooksDetectOrdinaryAndAlignedMemory) {
    allocations = releases = 0; watching = true;
    auto* p = ::operator new(64); ::operator delete(p);
    p = ::operator new(64, std::align_val_t{64}); ::operator delete(p, std::align_val_t{64});
    watching = false;
    EXPECT_EQ(allocations, 2u); EXPECT_EQ(releases, 2u);
}
TEST(AudioCoreRtTest, PreparedOperationsDoNotAllocateOrFree) {
    AudioBufferPool<4, 256> pool;
    SpscQueue<std::uint64_t, 4> queue;
    bool correct = true;
    allocations = releases = 0;
    const auto start = std::chrono::steady_clock::now();
    watching = true;
    for (std::uint64_t i = 0; i < 100000; ++i) {
        decltype(pool)::Handle handle;
        correct &= pool.acquire(static_cast<std::size_t>(i % 256 + 1), 2, handle);
        auto view = pool.view(handle);
        if (view.frameCount) view.channels[0][0] = 0.5f;
        correct &= queue.tryPush(i);
        std::uint64_t value = 0;
        correct &= queue.tryPop(value); correct &= value == i;
        correct &= pool.release(handle);
    }
    watching = false;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(correct); EXPECT_EQ(allocations, 0u); EXPECT_EQ(releases, 0u);
    RecordProperty("prepared_operations_100000_us", std::to_string(elapsed));
}
}
