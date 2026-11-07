#include "config/Profile.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>

namespace shirane {
namespace {
using Json = nlohmann::json;
constexpr std::array<const char*, 5> kIds{"mic", "game", "discord", "browser", "music"};
bool validGain(float value) noexcept { return std::isfinite(value) && value >= -60.0f && value <= 6.0f; }
bool validSend(const SendSettings& value) noexcept {
    return validGain(value.gainDb) && std::isfinite(value.pan) && value.pan >= -1.0f && value.pan <= 1.0f;
}
Json encodeSend(const SendSettings& value) {
    return {{"gainDb", value.gainDb}, {"pan", value.pan}, {"mute", value.mute}, {"solo", value.solo}};
}
SendSettings decodeSend(const Json& value) {
    return {value.at("gainDb").get<float>(), value.at("pan").get<float>(),
        value.at("mute").get<bool>(), value.at("solo").get<bool>()};
}
}
Profile defaultProfile() {
    Profile profile;
    constexpr std::array<const char*, 5> names{"マイク", "ゲーム", "Discord", "ブラウザー", "音楽"};
    for (std::size_t i = 0; i < profile.channels.size(); ++i) {
        profile.channels[i].id = kIds[i];
        profile.channels[i].name = names[i];
    }
    return profile;
}
bool validProfile(const Profile& profile) noexcept {
    if (!validGain(profile.monitorMasterDb) || !validGain(profile.streamMasterDb) || profile.layout.size() > 262144) return false;
    for (std::size_t i = 0; i < profile.channels.size(); ++i) {
        const auto& channel = profile.channels[i];
        if (channel.id != kIds[i] || channel.name.empty() || channel.name.size() > 128 ||
            !validSend(channel.monitor) || !validSend(channel.stream)) return false;
    }
    return true;
}
StoreResult loadProfile(const std::filesystem::path& path, Profile& destination) {
    try {
        if (!std::filesystem::exists(path)) return {StoreCode::Missing, "設定ファイルはまだありません"};
        if (std::filesystem::file_size(path) > 1024 * 1024) return {StoreCode::Invalid, "設定ファイルが大きすぎます"};
        std::ifstream input(path, std::ios::binary);
        if (!input) return {StoreCode::IoError, "設定ファイルを開けません"};
        const auto document = Json::parse(input);
        if (!document.at("schemaVersion").is_number_integer() || document.at("schemaVersion") != Profile::kSchemaVersion)
            return {StoreCode::UnsupportedVersion, "未対応の設定バージョンです。元のファイルを保持しました"};
        Profile candidate;
        const auto& channels = document.at("channels");
        if (!channels.is_array() || channels.size() != candidate.channels.size()) return {StoreCode::Invalid, "入力数が不正です"};
        for (std::size_t i = 0; i < candidate.channels.size(); ++i) {
            const auto& channel = channels.at(i);
            candidate.channels[i] = {channel.at("id").get<std::string>(), channel.at("name").get<std::string>(),
                decodeSend(channel.at("monitor")), decodeSend(channel.at("stream"))};
        }
        candidate.monitorMasterDb = document.at("monitorMasterDb").get<float>();
        candidate.streamMasterDb = document.at("streamMasterDb").get<float>();
        candidate.layout = document.at("layout").get<std::string>();
        if (!validProfile(candidate)) return {StoreCode::Invalid, "設定値が範囲外です"};
        destination = std::move(candidate);
        return {};
    } catch (const Json::exception&) {
        return {StoreCode::Invalid, "JSON設定が不正です。元のファイルを保持しました"};
    } catch (const std::exception&) {
        return {StoreCode::IoError, "設定ファイルの読込みに失敗しました"};
    }
}
StoreResult saveProfile(const std::filesystem::path& path, const Profile& profile) {
    if (!validProfile(profile)) return {StoreCode::Invalid, "不正な設定は保存できません"};
    auto temporary = path;
    temporary += L".tmp";
    try {
        Json channels = Json::array();
        for (const auto& channel : profile.channels)
            channels.push_back({{"id", channel.id}, {"name", channel.name},
                {"monitor", encodeSend(channel.monitor)}, {"stream", encodeSend(channel.stream)}});
        const Json document{{"schemaVersion", Profile::kSchemaVersion}, {"channels", channels},
            {"monitorMasterDb", profile.monitorMasterDb}, {"streamMasterDb", profile.streamMasterDb}, {"layout", profile.layout}};
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return {StoreCode::IoError, "一時設定ファイルを開けません"};
        output << document.dump(2) << '\n';
        output.flush();
        if (!output) return {StoreCode::IoError, "設定の書込みに失敗しました"};
        output.close();
        if (output.fail()) return {StoreCode::IoError, "設定ファイルを閉じられません"};
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return {StoreCode::IoError, "設定の置換に失敗しました。元の設定は保持されています"};
        return {};
    } catch (const std::exception&) { return {StoreCode::IoError, "設定の保存に失敗しました"}; }
}
}
