#include "audio/core/AudioClock.h"
#include "audio/dsp/GainProcessor.h"
#include "audio/dsp/Meter.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <numbers>
#include <thread>

namespace {
using namespace shirane::audio;
TEST(AudioClockTest, ExactEightHourTimelineAndEpoch) {
    AudioClock clock;
    constexpr std::uint64_t frames = 48000ull * 60 * 60 * 8;
    for (std::uint64_t f = 0; f < frames; f += 256) ASSERT_TRUE(clock.advance(256));
    EXPECT_EQ(clock.position().frames, frames);
    std::uint64_t ticks = 0; ASSERT_TRUE(clock.time100ns(ticks));
    EXPECT_EQ(ticks, 288000000000ull);
    ASSERT_TRUE(clock.restartEpoch()); EXPECT_EQ(clock.position().epoch, 1u); EXPECT_EQ(clock.position().frames, 0u);
    ASSERT_TRUE(clock.advance(1)); ASSERT_TRUE(clock.time100ns(ticks)); EXPECT_EQ(ticks, 208u);
    ASSERT_TRUE(clock.advance(47)); ASSERT_TRUE(clock.time100ns(ticks)); EXPECT_EQ(ticks, 10000u);
}
TEST(AudioClockTest, RejectsOverflowAndInvalidAdvanceWithoutMutation) {
    AudioClock clock;
    EXPECT_FALSE(clock.advance(257)); EXPECT_EQ(clock.position().frames, 0u);
    ASSERT_TRUE(clock.restartEpoch(std::numeric_limits<std::uint64_t>::max() - 1));
    EXPECT_FALSE(clock.advance(2)); EXPECT_EQ(clock.position().frames, std::numeric_limits<std::uint64_t>::max() - 1);
    EXPECT_TRUE(clock.advance(1)); EXPECT_FALSE(clock.advance(1));
    std::uint64_t ticks = 7; EXPECT_FALSE(clock.time100ns(ticks)); EXPECT_EQ(ticks, 7u);
    EXPECT_TRUE(clock.restartEpoch(48000)); EXPECT_EQ(clock.position().epoch, 2u);
    ASSERT_TRUE(clock.time100ns(ticks)); EXPECT_EQ(ticks, 10000000u);
}
TEST(GainProcessorTest, CommonInterfaceRampsStereoAndHasZeroLatency) {
    GainProcessor gain; IAudioProcessor& processor = gain;
    std::array<float, 256> left{}, right{};
    AudioBlockView block{{left, right}, 120, 2};
    left.fill(1); right.fill(-1);
    EXPECT_EQ(processor.process(block), ProcessorStatus::NotPrepared); EXPECT_EQ(left[0], 1);
    ASSERT_EQ(processor.prepare({48000, 256, 2}), ProcessorStatus::Ok);
    float target = 0; ASSERT_TRUE(GainProcessor::gainFromDb(0, target));
    ASSERT_EQ(gain.applyGain(target), ProcessorStatus::Ok);
    ASSERT_EQ(processor.process(block), ProcessorStatus::Ok);
    EXPECT_NEAR(left[119], 0.5f, 2e-6f); EXPECT_NEAR(right[119], -0.5f, 2e-6f);
    left.fill(1); right.fill(-1); ASSERT_EQ(processor.process(block), ProcessorStatus::Ok);
    EXPECT_EQ(left[119], 1); EXPECT_EQ(right[119], -1);
    left.fill(0); right.fill(0); left[0] = 1;
    ASSERT_EQ(processor.process(block), ProcessorStatus::Ok); EXPECT_EQ(left[0], 1); EXPECT_EQ(left[1], 0);
    EXPECT_EQ(processor.latencyFrames(), 0u);
    processor.reset(); left.fill(1); ASSERT_EQ(processor.process(block), ProcessorStatus::Ok); EXPECT_EQ(left[0], 0);
}
TEST(GainProcessorTest, RejectsParametersSpecsAndOverlappingPlanes) {
    GainProcessor gain;
    ASSERT_EQ(gain.prepare({48000, 256, 2}), ProcessorStatus::Ok); ASSERT_EQ(gain.applyGain(1), ProcessorStatus::Ok);
    for (auto spec : {ProcessSpec{44100, 256, 2}, ProcessSpec{48000, 257, 2}, ProcessSpec{48000, 0, 2}, ProcessSpec{48000, 256, 3}})
        EXPECT_EQ(gain.prepare(spec), ProcessorStatus::InvalidSpec);
    for (float value : {-1.0f, 3.0f, 0.0001f, std::numeric_limits<float>::quiet_NaN()})
        EXPECT_EQ(gain.applyGain(value), ProcessorStatus::InvalidParameter);
    float amplitude = 1; EXPECT_FALSE(GainProcessor::gainFromDb(7, amplitude)); EXPECT_EQ(amplitude, 1);
    std::array<float, 256> samples{}; samples.fill(1);
    EXPECT_EQ(gain.process({{samples, samples}, 256, 2}), ProcessorStatus::InvalidBlock);
    EXPECT_EQ(gain.process({{samples, {}}, 256, 1}), ProcessorStatus::InvalidBlock);
    EXPECT_EQ(samples[0], 1);
}
TEST(GainProcessorTest, NonFiniteAndOverflowCannotEscape) {
    GainProcessor gain; ASSERT_EQ(gain.prepare({48000, 256, 1}), ProcessorStatus::Ok);
    ASSERT_EQ(gain.applyGain(2), ProcessorStatus::Ok);
    std::array<float, 256> samples{};
    ASSERT_EQ(gain.process({{samples, {}}, 256, 1}), ProcessorStatus::Ok);
    samples.fill(1); samples[10] = std::numeric_limits<float>::infinity();
    EXPECT_EQ(gain.process({{samples, {}}, 256, 1}), ProcessorStatus::NonFiniteInput);
    for (auto sample : samples) EXPECT_EQ(sample, 0);
    samples.fill(std::numeric_limits<float>::max());
    EXPECT_EQ(gain.process({{samples, {}}, 256, 1}), ProcessorStatus::Overflow);
    for (auto sample : samples) EXPECT_EQ(sample, 0);
    samples.fill(std::numeric_limits<float>::denorm_min());
    EXPECT_EQ(gain.process({{samples, {}}, 256, 1}), ProcessorStatus::Ok);
    for (auto sample : samples) EXPECT_EQ(sample, 0);
}
void constantWindow(Meter& meter, float value) {
    std::array<float, 256> samples{}; samples.fill(value);
    for (std::size_t f = 0; f < Meter::kWindowFrames; f += 256)
        ASSERT_EQ(meter.process({{samples, {}}, std::min<std::size_t>(256, Meter::kWindowFrames - f), 1}), ProcessorStatus::Ok);
}
TEST(MeterTest, SilenceImpulseAndSineHaveExpectedPeakAndRms) {
    Meter meter; ASSERT_EQ(meter.prepare({48000, 256, 2}), ProcessorStatus::Ok);
    std::array<float, 256> left{}, right{};
    for (std::size_t offset = 0; offset < 2400; offset += 128) {
        const auto frames = std::min<std::size_t>(128, 2400 - offset);
        for (std::size_t f = 0; f < frames; ++f) {
            left[f] = offset + f == 0 ? 1.0f : 0;
            right[f] = static_cast<float>(std::sin(2 * std::numbers::pi * static_cast<double>(offset + f) / 48));
        }
        ASSERT_EQ(meter.process({{left, right}, frames, 2}), ProcessorStatus::Ok);
    }
    MeterSnapshot snapshot; ASSERT_TRUE(meter.tryRead(snapshot));
    EXPECT_EQ(snapshot.peak[0], 1); EXPECT_NEAR(snapshot.rms[0], 1 / std::sqrt(2400.0f), 1e-7f);
    EXPECT_NEAR(snapshot.peak[1], 1, 1e-6f); EXPECT_NEAR(snapshot.rms[1], std::sqrt(0.5f), 1e-6f);
    EXPECT_EQ(snapshot.sequence, 1u); EXPECT_EQ(snapshot.channelCount, 2u);
    left.fill(0); right.fill(0);
    for (int i = 0; i < 10; ++i) ASSERT_EQ(meter.process({{left, right}, 240, 2}), ProcessorStatus::Ok);
    ASSERT_TRUE(meter.tryRead(snapshot)); EXPECT_EQ(snapshot.peak[0], 0); EXPECT_EQ(snapshot.rms[1], 0);
}
TEST(MeterTest, WindowIsIndependentOfBlockPartition) {
    Meter first, second;
    ASSERT_EQ(first.prepare({48000, 256, 1}), ProcessorStatus::Ok);
    ASSERT_EQ(second.prepare({48000, 256, 1}), ProcessorStatus::Ok);
    constantWindow(first, 0.25f);
    std::array<float, 1> sample{0.25f};
    for (int i = 0; i < 2399; ++i) ASSERT_EQ(second.process({{sample, {}}, 1, 1}), ProcessorStatus::Ok);
    MeterSnapshot a, b; EXPECT_FALSE(second.tryRead(b));
    ASSERT_EQ(second.process({{sample, {}}, 1, 1}), ProcessorStatus::Ok);
    ASSERT_TRUE(first.tryRead(a)); ASSERT_TRUE(second.tryRead(b)); EXPECT_EQ(a.peak, b.peak); EXPECT_EQ(a.rms, b.rms);
}
TEST(MeterTest, InvalidSamplesAreCountedAndQueueOverflowDoesNotBlock) {
    Meter meter; ASSERT_EQ(meter.prepare({48000, 256, 1}), ProcessorStatus::Ok);
    std::array<float, 256> samples{};
    samples[0] = std::numeric_limits<float>::quiet_NaN(); samples[1] = std::numeric_limits<float>::infinity();
    ASSERT_EQ(meter.process({{samples, {}}, 256, 1}), ProcessorStatus::Ok);
    samples.fill(0);
    for (int i = 0; i < 8; ++i) ASSERT_EQ(meter.process({{samples, {}}, 256, 1}), ProcessorStatus::Ok);
    ASSERT_EQ(meter.process({{samples, {}}, 96, 1}), ProcessorStatus::Ok);
    MeterSnapshot snapshot; ASSERT_TRUE(meter.tryRead(snapshot)); EXPECT_EQ(snapshot.invalidSamples, 2u); EXPECT_EQ(snapshot.rms[0], 0);
    for (int i = 0; i < 10; ++i) constantWindow(meter, 0.5f);
    for (int i = 0; i < 8; ++i) ASSERT_TRUE(meter.tryRead(snapshot));
    EXPECT_FALSE(meter.tryRead(snapshot));
    constantWindow(meter, std::numeric_limits<float>::max()); ASSERT_TRUE(meter.tryRead(snapshot));
    EXPECT_EQ(snapshot.droppedWindows, 2u); EXPECT_EQ(snapshot.sequence, 12u); EXPECT_TRUE(std::isfinite(snapshot.rms[0]));
}
TEST(MeterTest, RejectsUnpreparedMalformedAndRepeatedPrepare) {
    Meter meter; std::array<float, 256> samples{};
    EXPECT_EQ(meter.process({{samples, {}}, 256, 1}), ProcessorStatus::NotPrepared);
    EXPECT_EQ(meter.prepare({44100, 256, 1}), ProcessorStatus::InvalidSpec);
    ASSERT_EQ(meter.prepare({48000, 128, 1}), ProcessorStatus::Ok);
    EXPECT_EQ(meter.prepare({48000, 128, 1}), ProcessorStatus::InvalidSpec);
    EXPECT_EQ(meter.process({{samples, {}}, 129, 1}), ProcessorStatus::InvalidBlock);
    EXPECT_EQ(meter.process({{}, 1, 1}), ProcessorStatus::InvalidBlock);
    EXPECT_EQ(meter.process({{}, 0, 1}), ProcessorStatus::Ok);
}
TEST(MeterTest, ConcurrentReaderReceivesCoherentSnapshots) {
    Meter meter; ASSERT_EQ(meter.prepare({48000, 256, 1}), ProcessorStatus::Ok);
    std::atomic<bool> done{false};
    std::thread producer([&] {
        std::array<float, 240> samples{};
        for (unsigned window = 1; window <= 1000; ++window) {
            samples.fill(window % 2 ? 0.25f : 0.5f);
            for (int i = 0; i < 10; ++i) (void)meter.process({{samples, {}}, 240, 1});
        }
        done.store(true);
    });
    std::uint64_t previous = 0; bool correct = true;
    const auto read = [&](const MeterSnapshot& snapshot) {
        const float expected = snapshot.sequence % 2 ? 0.25f : 0.5f;
        correct &= snapshot.sequence > previous && snapshot.peak[0] == expected && snapshot.rms[0] == expected;
        previous = snapshot.sequence;
    };
    MeterSnapshot snapshot;
    while (!done.load()) { if (meter.tryRead(snapshot)) read(snapshot); else std::this_thread::yield(); }
    while (meter.tryRead(snapshot)) read(snapshot);
    producer.join(); EXPECT_TRUE(correct); EXPECT_GT(previous, 0u);
}
}
