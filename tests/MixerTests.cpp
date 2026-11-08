#include "audio/mixer/Mixer.h"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <numbers>

namespace {
using namespace shirane::audio;
constexpr float kCenter = 0.70710678118f;
struct MixerTest : testing::Test {
    Mixer mixer;
    MixerParameters parameters;
    std::array<float, 256> left{}, right{};
    MixerInput input{0, 1, {left, {}}};
    void apply() {
        PreparedMixerParameters prepared;
        ASSERT_TRUE(PreparedMixerParameters::prepare(parameters, prepared));
        mixer.apply(prepared);
    }
    void settle() { ASSERT_EQ(mixer.process({}, 240).status, MixStatus::Ok); }
    void process(std::size_t frames = 256) { ASSERT_EQ(mixer.process({&input, 1}, frames).status, MixStatus::Ok); }
};
TEST_F(MixerTest, DefaultSilenceAndMonoCenterImpulse) {
    left[0] = 1;
    process();
    for (float sample : mixer.output(Bus::Monitor, 0)) EXPECT_EQ(sample, 0);
    parameters.sources[0][0].mute = false; apply(); settle(); process();
    EXPECT_NEAR(mixer.output(Bus::Monitor, 0)[0], kCenter, 1e-6f);
    EXPECT_NEAR(mixer.output(Bus::Monitor, 1)[0], kCenter, 1e-6f);
    for (std::size_t i = 1; i < 256; ++i) EXPECT_EQ(mixer.output(Bus::Monitor, 0)[i], 0);
    for (float sample : mixer.output(Bus::Stream, 0)) EXPECT_EQ(sample, 0);
}
TEST_F(MixerTest, SineStereoPanAndHeadroom) {
    input.channelCount = 2; input.channels[1] = right;
    for (std::size_t i = 0; i < 256; ++i) {
        left[i] = std::sin(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / 48.0f);
        right[i] = -left[i];
    }
    parameters.sources[0][0] = {6, -1, false, false}; apply(); settle(); process();
    const float gain = std::pow(10.0f, 6.0f / 20.0f);
    for (std::size_t i = 0; i < 256; ++i) {
        EXPECT_NEAR(mixer.output(Bus::Monitor, 0)[i], left[i] * gain, 1e-6f);
        EXPECT_EQ(mixer.output(Bus::Monitor, 1)[i], 0);
    }
    EXPECT_GT(mixer.output(Bus::Monitor, 0)[12], 1);
    parameters.sources[0][0].pan = 1; apply(); settle(); process();
    for (std::size_t i = 0; i < 256; ++i) {
        EXPECT_EQ(mixer.output(Bus::Monitor, 0)[i], 0);
        EXPECT_NEAR(mixer.output(Bus::Monitor, 1)[i], right[i] * gain, 1e-6f);
    }
}
TEST_F(MixerTest, BusLocalSoloMuteAndMasterAreIndependent) {
    left.fill(1); right.fill(2);
    std::array<MixerInput, 2> inputs{input, MixerInput{1, 1, {right, {}}}};
    for (std::size_t s = 0; s < 2; ++s) for (auto& send : parameters.sources[s]) send = {0, -1, false, false};
    parameters.sources[0][0].solo = true;
    apply(); settle(); ASSERT_EQ(mixer.process(inputs, 256).status, MixStatus::Ok);
    EXPECT_EQ(mixer.output(Bus::Monitor, 0)[255], 1);
    EXPECT_EQ(mixer.output(Bus::Stream, 0)[255], 3);
    parameters.sources[0][0].mute = true; parameters.masterDb[0] = -6;
    apply(); settle(); ASSERT_EQ(mixer.process(inputs, 256).status, MixStatus::Ok);
    EXPECT_EQ(mixer.output(Bus::Monitor, 0)[255], 0); // Muted solo still isolates this bus.
    EXPECT_EQ(mixer.output(Bus::Stream, 0)[255], 3);
    parameters.sources[0][0].mute = false;
    apply(); settle(); ASSERT_EQ(mixer.process(inputs, 256).status, MixStatus::Ok);
    EXPECT_NEAR(mixer.output(Bus::Monitor, 0)[255], std::pow(10.0f, -6.0f / 20.0f), 1e-6f);
}
TEST_F(MixerTest, RampIsContinuousAcrossBlocksAndRepeatedApply) {
    left.fill(1); parameters.sources[0][0] = {0, -1, false, false}; apply();
    float previous = 0;
    for (int block = 0; block < 4; ++block) {
        apply(); process(60);
        for (float sample : mixer.output(Bus::Monitor, 0)) {
            EXPECT_NEAR(sample - previous, 1.0f / 240.0f, 2e-6f); previous = sample;
        }
    }
    EXPECT_EQ(previous, 1);
    parameters.sources[0][0].mute = true; apply(); process(120);
    EXPECT_NEAR(mixer.output(Bus::Monitor, 0)[119], 0.5f, 2e-6f);
    parameters.sources[0][0].mute = false; apply(); process(240);
    EXPECT_NEAR(mixer.output(Bus::Monitor, 0)[0], 0.5f + 0.5f / 240, 2e-6f);
    EXPECT_EQ(mixer.output(Bus::Monitor, 0)[239], 1);
}
TEST_F(MixerTest, VariableBlockPartitionDoesNotChangeSignal) {
    left.fill(1); parameters.sources[0][0] = {0, 0, false, false}; apply();
    Mixer whole;
    PreparedMixerParameters prepared; ASSERT_TRUE(PreparedMixerParameters::prepare(parameters, prepared));
    whole.apply(prepared); ASSERT_EQ(whole.process({&input, 1}, 256).status, MixStatus::Ok);
    std::size_t offset = 0;
    for (std::size_t frames : {1u, 63u, 64u, 128u}) {
        process(frames);
        for (std::size_t f = 0; f < frames; ++f)
            EXPECT_EQ(mixer.output(Bus::Monitor, 0)[f], whole.output(Bus::Monitor, 0)[offset + f]);
        offset += frames;
    }
}
TEST_F(MixerTest, EmergencyMuteLatchesAndFinishesIn48Samples) {
    left.fill(1);
    for (auto& send : parameters.sources[0]) send = {0, -1, false, false};
    apply(); settle(); mixer.requestEmergencyMute(); process(24);
    EXPECT_NEAR(mixer.output(Bus::Monitor, 0)[23], 0.5f, 1e-6f);
    apply(); process(24);
    EXPECT_EQ(mixer.output(Bus::Monitor, 0)[23], 0);
    process(); for (auto bus : {Bus::Monitor, Bus::Stream})
        for (float sample : mixer.output(bus, 0)) EXPECT_EQ(sample, 0);
    mixer.reset(); apply(); settle(); process(); EXPECT_EQ(mixer.output(Bus::Monitor, 0)[255], 1);
}
TEST_F(MixerTest, NonFiniteSourceIsSilencedAndDenormalsAreFlushed) {
    left.fill(1); right.fill(2);
    std::array<MixerInput, 2> inputs{input, MixerInput{1, 1, {right, {}}}};
    for (std::size_t s = 0; s < 2; ++s) for (auto& send : parameters.sources[s]) send = {0, -1, false, false};
    apply(); settle();
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        left[100] = invalid;
        const auto result = mixer.process(inputs, 256);
        EXPECT_EQ(result.invalidSources, 1u);
        for (auto bus : {Bus::Monitor, Bus::Stream}) for (float sample : mixer.output(bus, 0)) EXPECT_EQ(sample, 2);
    }
    left.fill(std::numeric_limits<float>::denorm_min()); process();
    for (float sample : mixer.output(Bus::Monitor, 0)) EXPECT_EQ(sample, 0);
    left.fill(std::numeric_limits<float>::max()); parameters.sources[0][0].gainDb = 6;
    apply(); settle(); EXPECT_GT(mixer.process({&input, 1}, 256).overflowSamples, 0u);
    for (float sample : mixer.output(Bus::Monitor, 0)) EXPECT_TRUE(std::isfinite(sample));
}
TEST_F(MixerTest, InvalidConfigurationPreservesPreparedTargets) {
    parameters.sources[0][0] = {0, -1, false, false};
    PreparedMixerParameters prepared; ASSERT_TRUE(PreparedMixerParameters::prepare(parameters, prepared));
    auto bad = parameters; bad.sources[15][1].pan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(PreparedMixerParameters::prepare(bad, prepared));
    bad = parameters; bad.masterDb[0] = 7; EXPECT_FALSE(PreparedMixerParameters::prepare(bad, prepared));
    bad = parameters; bad.sources[0][0].gainDb = -61; EXPECT_FALSE(PreparedMixerParameters::prepare(bad, prepared));
    mixer.apply(prepared); settle(); left.fill(1); process(); EXPECT_EQ(mixer.output(Bus::Monitor, 0)[255], 1);
}
TEST_F(MixerTest, InvalidShapesDuplicatesAndFeedbackReturnSilence) {
    left.fill(1); parameters.sources[0][0] = {0, -1, false, false}; apply(); settle(); process();
    std::array<MixerInput, 2> duplicates{input, input};
    EXPECT_EQ(mixer.process(duplicates, 256).status, MixStatus::InvalidInput);
    for (float sample : mixer.output(Bus::Monitor, 0)) EXPECT_EQ(sample, 0);
    input.source = 16; EXPECT_EQ(mixer.process({&input, 1}, 256).status, MixStatus::InvalidInput);
    input.source = 0; input.channelCount = 3; EXPECT_EQ(mixer.process({&input, 1}, 256).status, MixStatus::InvalidInput);
    input.channelCount = 1; input.channels[0] = std::span<const float>(left).first(5);
    EXPECT_EQ(mixer.process({&input, 1}, 6).status, MixStatus::InvalidInput);
    EXPECT_EQ(mixer.process({}, 257).status, MixStatus::InvalidInput); EXPECT_TRUE(mixer.output(Bus::Monitor, 0).empty());
    process(0); EXPECT_TRUE(mixer.output(Bus::Monitor, 0).empty());
    input.channels[0] = left; process(); input.channels[0] = mixer.output(Bus::Monitor, 0);
    EXPECT_EQ(mixer.process({&input, 1}, 256).status, MixStatus::InvalidInput);
}
}
