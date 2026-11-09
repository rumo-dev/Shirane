#include "audio/core/AudioBufferPool.h"
#include "audio/core/SpscQueue.h"
#include "audio/mixer/Mixer.h"
#include "audio/core/AudioClock.h"
#include "audio/dsp/GainProcessor.h"
#include "audio/dsp/Meter.h"
#include <algorithm>
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
TEST(AudioCoreRtTest, MixerSixteenSourcesDoesNotAllocateAndRecordsTiming) {
    Mixer mixer;
    MixerParameters parameters;
    for (auto& source : parameters.sources) for (auto& send : source) send.mute = false;
    PreparedMixerParameters prepared;
    ASSERT_TRUE(PreparedMixerParameters::prepare(parameters, prepared));
    std::array<float, 128> samples{}; samples.fill(0.01f);
    std::array<MixerInput, 16> inputs{};
    for (std::size_t s = 0; s < inputs.size(); ++s) inputs[s] = {s, 2, {samples, samples}};
    mixer.apply(prepared);
    ASSERT_EQ(mixer.process(inputs, 128).status, MixStatus::Ok);
    std::array<long long, 1000> nanoseconds{};
    allocations = releases = 0;
    bool correct = true;
    for (auto& elapsed : nanoseconds) {
        const auto start = std::chrono::steady_clock::now();
        watching = true;
        mixer.apply(prepared);
        const auto result = mixer.process(inputs, 128);
        watching = false;
        elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        correct &= result.status == MixStatus::Ok && result.invalidSources == 0 && result.overflowSamples == 0;
    }
    EXPECT_TRUE(correct); EXPECT_EQ(allocations, 0u); EXPECT_EQ(releases, 0u);
    EXPECT_GT(mixer.output(Bus::Monitor, 0)[127], 0.1f);
    std::sort(nanoseconds.begin(), nanoseconds.end());
    RecordProperty("mixer_16_stereo_128frames_p50_ns", std::to_string(nanoseconds[499]));
    RecordProperty("mixer_16_stereo_128frames_p99_ns", std::to_string(nanoseconds[989]));
    RecordProperty("mixer_16_stereo_128frames_max_ns", std::to_string(nanoseconds.back()));
}
TEST(AudioCoreRtTest, GainMixerMeterClockChainHasNoAllocation) {
    GainProcessor gain; IAudioProcessor& processor = gain;
    Meter meter; Mixer mixer; AudioClock clock;
    ASSERT_EQ(processor.prepare({48000, 256, 2}), ProcessorStatus::Ok);
    ASSERT_EQ(gain.applyGain(1), ProcessorStatus::Ok);
    ASSERT_EQ(meter.prepare({48000, 256, 2}), ProcessorStatus::Ok);
    MixerParameters parameters;
    for (auto& send : parameters.sources[0]) send = {0, -1, false, false};
    PreparedMixerParameters prepared; ASSERT_TRUE(PreparedMixerParameters::prepare(parameters, prepared));
    mixer.apply(prepared);
    std::array<float, 128> left{}, right{};
    const MixerInput input{0, 2, {left, right}};
    std::array<long long, 1000> nanoseconds{};
    allocations = releases = 0; bool correct = true; unsigned windows = 0;
    MeterSnapshot snapshot;
    for (auto& elapsed : nanoseconds) {
        left.fill(0.1f); right.fill(0.2f);
        const auto start = std::chrono::steady_clock::now();
        watching = true;
        correct &= processor.process({{left, right}, 128, 2}) == ProcessorStatus::Ok;
        correct &= mixer.process({&input, 1}, 128).status == MixStatus::Ok;
        correct &= meter.process({{mixer.output(Bus::Monitor, 0), mixer.output(Bus::Monitor, 1)}, 128, 2}) == ProcessorStatus::Ok;
        correct &= clock.advance(128);
        watching = false;
        elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        if (meter.tryRead(snapshot)) ++windows;
    }
    EXPECT_TRUE(correct); EXPECT_EQ(allocations, 0u); EXPECT_EQ(releases, 0u);
    EXPECT_EQ(clock.position().frames, 128000u); EXPECT_EQ(windows, 53u);
    EXPECT_NEAR(snapshot.rms[0], 0.1f, 1e-6f); EXPECT_EQ(snapshot.rms[1], 0);
    std::sort(nanoseconds.begin(), nanoseconds.end());
    RecordProperty("gain_mixer_meter_clock_128frames_p50_ns", std::to_string(nanoseconds[499]));
    RecordProperty("gain_mixer_meter_clock_128frames_p99_ns", std::to_string(nanoseconds[989]));
    RecordProperty("gain_mixer_meter_clock_128frames_max_ns", std::to_string(nanoseconds.back()));
}
// Explicit real wall-clock run; excluded from ordinary unit-test discovery.
TEST(CoreEnduranceTest, DISABLED_ContinuousTransfer) {
    struct Packet {
        std::uint64_t sequence = 0, firstFrame = 0;
        std::uint32_t frames = 0;
        std::array<std::array<float, 256>, 2> samples{};
    };
    SpscQueue<Packet, 64> queue;
    AudioBufferPool<2, 256> pool;
    GainProcessor gain; Mixer mixer; Meter meter; AudioClock clock;
    ASSERT_EQ(gain.prepare({48000, 256, 2}), ProcessorStatus::Ok);
    ASSERT_EQ(gain.applyGain(1), ProcessorStatus::Ok);
    ASSERT_EQ(meter.prepare({48000, 256, 2}), ProcessorStatus::Ok);
    decltype(pool)::Handle handle;
    ASSERT_TRUE(pool.acquire(240, 2, handle));
    ASSERT_EQ(gain.process(pool.view(handle)), ProcessorStatus::Ok);
    ASSERT_TRUE(pool.release(handle));
    MixerParameters parameters;
    parameters.sources[0][0] = {0, -1, false, false};
    parameters.sources[0][1] = {0, 1, false, false};
    PreparedMixerParameters prepared;
    ASSERT_TRUE(PreparedMixerParameters::prepare(parameters, prepared));
    mixer.apply(prepared); ASSERT_EQ(mixer.process({}, 240).status, MixStatus::Ok);
    std::atomic<bool> done{false}, fullObserved{false}, abort{false};
    std::uint64_t sent = 0, sentFrames = 0, fullRetries = 0;
    unsigned producerAllocations = 0, producerReleases = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(60);
    std::thread producer([&] {
        Packet packet;
        constexpr std::array<std::uint32_t, 4> sizes{1, 64, 128, 256};
        allocations = releases = 0; watching = true;
        while (!abort.load() && std::chrono::steady_clock::now() < deadline) {
            packet.sequence = sent; packet.firstFrame = sentFrames; packet.frames = sizes[sent % sizes.size()];
            for (std::uint32_t f = 0; f < packet.frames; ++f) {
                const float value = static_cast<float>((sentFrames + f) % 4) * 0.125f;
                packet.samples[0][f] = value; packet.samples[1][f] = -2 * value;
            }
            bool pushed = false;
            while (!(pushed = queue.tryPush(packet))) {
                ++fullRetries; fullObserved.store(true);
                if (abort.load() || std::chrono::steady_clock::now() >= deadline) break;
                std::this_thread::yield(); // Test driver only; queue operations never wait.
            }
            if (!pushed) break;
            ++sent; sentFrames += packet.frames;
        }
        watching = false;
        producerAllocations = allocations; producerReleases = releases;
        done.store(true);
    });
    // Force the full path before starting the consumer, with a bounded driver wait.
    while (!fullObserved.load() && std::chrono::steady_clock::now() < start + std::chrono::seconds(5))
        std::this_thread::yield();
    std::uint64_t received = 0, windows = 0;
    bool correct = true;
    Packet packet; MeterSnapshot snapshot;
    allocations = releases = 0; watching = true;
    while (std::chrono::steady_clock::now() < deadline + std::chrono::seconds(5)) {
        if (!queue.tryPop(packet)) {
            if (!done.load()) { std::this_thread::yield(); continue; }
            if (!queue.tryPop(packet)) break; // Acquire the last publication before declaring empty.
        }
        correct &= packet.sequence == received && packet.firstFrame == clock.position().frames;
        if (!pool.acquire(packet.frames, 2, handle)) { correct = false; break; }
        auto block = pool.view(handle);
        for (std::size_t c = 0; c < 2; ++c) std::copy_n(packet.samples[c].begin(), packet.frames, block.channels[c].begin());
        correct &= gain.process(block) == ProcessorStatus::Ok;
        const MixerInput input{0, 2, {block.channels[0], block.channels[1]}};
        const auto result = mixer.process({&input, 1}, packet.frames);
        correct &= result.status == MixStatus::Ok && !result.invalidSources && !result.overflowSamples;
        for (std::uint32_t f = 0; f < packet.frames; ++f) {
            const float expected = static_cast<float>((clock.position().frames + f) % 4) * 0.125f;
            correct &= mixer.output(Bus::Monitor, 0)[f] == expected && mixer.output(Bus::Monitor, 1)[f] == 0;
            correct &= mixer.output(Bus::Stream, 0)[f] == 0 && mixer.output(Bus::Stream, 1)[f] == -2 * expected;
        }
        correct &= meter.process({{mixer.output(Bus::Monitor, 0), mixer.output(Bus::Monitor, 1)}, packet.frames, 2}) == ProcessorStatus::Ok;
        if (meter.tryRead(snapshot)) {
            ++windows;
            correct &= snapshot.sequence == windows && snapshot.droppedWindows == 0 && snapshot.invalidSamples == 0;
            correct &= snapshot.peak[0] == 0.375f && snapshot.rms[1] == 0;
            correct &= std::abs(snapshot.rms[0] - std::sqrt(0.0546875f)) < 1e-6f;
        }
        correct &= clock.advance(packet.frames) && pool.release(handle);
        ++received;
    }
    watching = false;
    abort.store(true); producer.join();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(correct); EXPECT_TRUE(fullObserved.load()); EXPECT_GT(fullRetries, 0u);
    EXPECT_EQ(sent, received); EXPECT_GT(received, 1000u);
    EXPECT_EQ(clock.position().frames, sentFrames); EXPECT_EQ(windows, sentFrames / Meter::kWindowFrames);
    EXPECT_EQ(pool.available(), 2u); EXPECT_FALSE(queue.tryPop(packet));
    EXPECT_EQ(allocations, 0u); EXPECT_EQ(releases, 0u);
    EXPECT_EQ(producerAllocations, 0u); EXPECT_EQ(producerReleases, 0u);
    EXPECT_GE(elapsedMs, 60000);
    RecordProperty("duration_ms", std::to_string(elapsedMs));
    RecordProperty("transferred_blocks", std::to_string(received));
    RecordProperty("transferred_frames", std::to_string(sentFrames));
    RecordProperty("full_retries", std::to_string(fullRetries));
    RecordProperty("meter_windows", std::to_string(windows));
}
}
