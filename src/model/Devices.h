#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace shirane {
enum class DeviceFlow { Capture, Render };
enum class DeviceState { Active, Disabled, Unplugged, NotPresent };
struct EndpointInfo {
    std::string id;
    std::string name;
    DeviceFlow flow = DeviceFlow::Render;
    DeviceState state = DeviceState::NotPresent;
    bool multimediaDefault = false;
};
struct SessionInfo {
    std::string endpointId;
    std::string instanceId;
    std::string name;
    std::uint32_t processId = 0;
    bool multipleProcesses = false;
    bool active = false;
    bool volumeAvailable = false;
    float volume = 0;
    bool mute = false;
};
enum class DeviceStatus { Loading, Ready, Partial, Failed };
struct DeviceSnapshot {
    std::uint64_t generation = 0;
    std::uint64_t notificationCount = 0;
    DeviceStatus status = DeviceStatus::Loading;
    std::int32_t nativeError = 0;
    std::string operation;
    bool notificationsAvailable = false;
    std::vector<EndpointInfo> endpoints;
    std::vector<SessionInfo> sessions;
};
struct DeviceSelection {
    std::string microphoneId;
    std::string monitorId;
    std::string streamId;
    bool operator==(const DeviceSelection&) const = default;
};
// Borrowed from the snapshot. No name/PID based substitution on missing devices.
inline const EndpointInfo* findEndpoint(const DeviceSnapshot& snapshot, const std::string& id, DeviceFlow flow) noexcept {
    if (id.empty()) return nullptr;
    for (const auto& endpoint : snapshot.endpoints)
        if (endpoint.id == id && endpoint.flow == flow) return &endpoint;
    return nullptr;
}
inline bool validDeviceSelection(const DeviceSelection& value) noexcept {
    const auto validId = [](const std::string& id) {
        return id.size() <= 4096 && id.find('\0') == std::string::npos;
    };
    return validId(value.microphoneId) && validId(value.monitorId) && validId(value.streamId) &&
        (value.monitorId.empty() || value.streamId.empty() || value.monitorId != value.streamId);
}
}
