#include "config/Profile.h"
#include "audio/core/AudioCore.h"
#include <gtest/gtest.h>
#include <Windows.h>
#include <fstream>
#include <limits>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
class ProfileTest : public testing::Test {
protected:
    std::filesystem::path directory;
    std::filesystem::path path;
    void SetUp() override {
        wchar_t* testRoot = nullptr;
        std::size_t length = 0;
        ASSERT_EQ(_wdupenv_s(&testRoot, &length, L"SHIRANE_TEST_ROOT"), 0);
        const auto root = testRoot ? std::filesystem::path(testRoot) : std::filesystem::temp_directory_path();
        std::free(testRoot);
        directory = root / ("shirane-test-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        path = directory / "profile.json";
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
};
TEST_F(ProfileTest, DefaultsAreMutedAndEngineUnavailable) {
    const auto profile = shirane::defaultProfile();
    ASSERT_TRUE(shirane::validProfile(profile));
    for (const auto& channel : profile.channels) { EXPECT_TRUE(channel.monitor.mute); EXPECT_TRUE(channel.stream.mute); }
    EXPECT_EQ(shirane::audio::engineState(), shirane::audio::EngineState::Unavailable);
}
TEST_F(ProfileTest, RoundTripIndependentBusesAndLayout) {
    auto original = shirane::defaultProfile();
    original.channels[0].monitor = {-12.5f, -0.4f, false, true};
    original.channels[0].stream = {3.0f, 0.6f, true, false};
    original.monitorMasterDb = -8.0f;
    original.layout = "[Window][ミキサー]\nPos=10,20\n";
    ASSERT_TRUE(shirane::saveProfile(path, original).ok());
    auto restored = shirane::defaultProfile();
    ASSERT_TRUE(shirane::loadProfile(path, restored).ok());
    EXPECT_EQ(original, restored);
    original.streamMasterDb = -5.0f;
    ASSERT_TRUE(shirane::saveProfile(path, original).ok());
    ASSERT_TRUE(shirane::loadProfile(path, restored).ok());
    EXPECT_EQ(original, restored);
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}
TEST_F(ProfileTest, CorruptAndMissingPreserveDestination) {
    auto profile = shirane::defaultProfile();
    profile.channels[2].stream.gainDb = -9;
    const auto before = profile;
    EXPECT_EQ(shirane::loadProfile(path, profile).code, shirane::StoreCode::Missing);
    { std::ofstream file(path); file << "{broken"; }
    EXPECT_EQ(shirane::loadProfile(path, profile).code, shirane::StoreCode::Invalid);
    EXPECT_EQ(before, profile);
}
TEST_F(ProfileTest, UnsupportedSchemasAreRejectedWithoutMutation) {
    auto profile = shirane::defaultProfile();
    const auto before = profile;
    for (const int version : {0, 2}) {
        { std::ofstream file(path); file << "{\"schemaVersion\":" << version << "}"; }
        EXPECT_EQ(shirane::loadProfile(path, profile).code, shirane::StoreCode::UnsupportedVersion);
        EXPECT_EQ(before, profile);
    }
}
TEST_F(ProfileTest, InvalidNumbersCannotOverwriteGoodFile) {
    const auto good = shirane::defaultProfile();
    ASSERT_TRUE(shirane::saveProfile(path, good).ok());
    for (const auto value : {7.0f, -61.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto bad = good; bad.channels[0].stream.gainDb = value;
        EXPECT_EQ(shirane::saveProfile(path, bad).code, shirane::StoreCode::Invalid);
    }
    auto loaded = good;
    ASSERT_TRUE(shirane::loadProfile(path, loaded).ok()); EXPECT_EQ(loaded, good);
}
TEST_F(ProfileTest, IdentityPanAndSizeBoundaries) {
    auto profile = shirane::defaultProfile();
    profile.channels[0].monitor.pan = 1.1f; EXPECT_FALSE(shirane::validProfile(profile));
    profile = shirane::defaultProfile(); profile.channels[1].id = "mic"; EXPECT_FALSE(shirane::validProfile(profile));
    profile = shirane::defaultProfile(); profile.layout.resize(262145); EXPECT_FALSE(shirane::validProfile(profile));
    profile = shirane::defaultProfile(); profile.channels[0].stream.gainDb = -60; profile.channels[1].monitor.gainDb = 6;
    EXPECT_TRUE(shirane::validProfile(profile));
}
TEST_F(ProfileTest, FailedReplacementPreservesExistingDestination) {
    std::filesystem::create_directory(path);
    { std::ofstream sentinel(path / "keep.txt"); sentinel << "preserved"; }
    EXPECT_EQ(shirane::saveProfile(path, shirane::defaultProfile()).code, shirane::StoreCode::IoError);
    EXPECT_TRUE(std::filesystem::exists(path / "keep.txt"));
}
TEST_F(ProfileTest, InvalidFileFieldsNeverPartiallyApply) {
    const auto original = shirane::defaultProfile();
    ASSERT_TRUE(shirane::saveProfile(path, original).ok());
    nlohmann::json valid;
    { std::ifstream input(path); input >> valid; }
    std::vector<nlohmann::json> invalid(7, valid);
    invalid[0]["channels"][0]["monitor"]["gainDb"] = 7;
    invalid[1]["channels"][4]["stream"]["pan"] = -2;
    invalid[2]["channels"][0]["monitor"]["mute"] = "false";
    invalid[3]["channels"][2]["id"] = "mic";
    invalid[4]["channels"].erase(4);
    invalid[5].erase("layout");
    invalid[6]["streamMasterDb"] = nullptr;
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        SCOPED_TRACE(i);
        { std::ofstream output(path); output << invalid[i]; }
        auto destination = original;
        destination.channels[1].stream.gainDb = -18;
        const auto before = destination;
        EXPECT_EQ(shirane::loadProfile(path, destination).code, shirane::StoreCode::Invalid);
        EXPECT_EQ(destination, before);
    }
}
TEST_F(ProfileTest, OversizedFileIsRejected) {
    { std::ofstream output(path, std::ios::binary); output << std::string(1024 * 1024 + 1, ' '); }
    auto profile = shirane::defaultProfile();
    EXPECT_EQ(shirane::loadProfile(path, profile).code, shirane::StoreCode::Invalid);
    EXPECT_EQ(profile, shirane::defaultProfile());
}
TEST_F(ProfileTest, UnicodeDirectoryRoundTrip) {
    const auto nested = directory / L"調音 設定" / L"プロファイル.json";
    const auto original = shirane::defaultProfile();
    ASSERT_TRUE(shirane::saveProfile(nested, original).ok());
    auto loaded = original;
    loaded.streamMasterDb = -20;
    ASSERT_TRUE(shirane::loadProfile(nested, loaded).ok());
    EXPECT_EQ(loaded, original);
}
}
