#pragma once
#include <array>
#include <filesystem>
#include <string>
#include "model/Devices.h"

namespace shirane {
struct SendSettings {
    float gainDb = 0.0f;
    float pan = 0.0f;
    bool mute = true;
    bool solo = false;
    bool operator==(const SendSettings&) const = default;
};
struct ChannelSettings {
    std::string id;
    std::string name;
    SendSettings monitor;
    SendSettings stream;
    bool operator==(const ChannelSettings&) const = default;
};
struct Profile {
    static constexpr int kSchemaVersion = 2;
    std::array<ChannelSettings, 5> channels;
    float monitorMasterDb = 0.0f;
    float streamMasterDb = 0.0f;
    std::string layout;
    DeviceSelection devices;
    bool operator==(const Profile&) const = default;
};
enum class StoreCode { Ok, Missing, Invalid, UnsupportedVersion, IoError };
struct StoreResult {
    StoreCode code = StoreCode::Ok;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return code == StoreCode::Ok; }
};
[[nodiscard]] Profile defaultProfile();
[[nodiscard]] bool validProfile(const Profile& profile) noexcept;
// Non-RT, single UI owner in Phase 0. Failed loads never modify destination.
[[nodiscard]] StoreResult loadProfile(const std::filesystem::path& path, Profile& destination);
[[nodiscard]] StoreResult saveProfile(const std::filesystem::path& path, const Profile& profile);
}
