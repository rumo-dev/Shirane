#pragma once
#include "model/Devices.h"

namespace shirane {
enum class DeviceCheckState {
    Idle, AwaitDisconnect, AwaitReconnect, Observed,
    SelectionChanged, EnumerationFailed, NotificationMissing, TimedOut
};
// Non-RT, UI-owned evidence recorder. Synthetic snapshots test the recorder,
// never certify the real hardware or identify the origin of a global notification.
class DeviceCheck {
public:
    bool start(const DeviceSnapshot& snapshot, const std::string& selectedId, double nowSeconds) {
        const auto* endpoint = findEndpoint(snapshot, selectedId, DeviceFlow::Capture);
        if (snapshot.status != DeviceStatus::Ready || !snapshot.notificationsAvailable ||
            !snapshot.generation || !endpoint || endpoint->state != DeviceState::Active) return false;
        id_ = selectedId;
        generation_ = snapshot.generation;
        count_ = snapshot.notificationCount;
        lastCount_ = count_;
        deadline_ = nowSeconds + 120.0;
        state_ = DeviceCheckState::AwaitDisconnect;
        return true;
    }
    void observe(const DeviceSnapshot& snapshot, const std::string& selectedId, double nowSeconds) noexcept {
        if (!running()) return;
        if (selectedId != id_) { state_ = DeviceCheckState::SelectionChanged; return; }
        if (nowSeconds >= deadline_) { state_ = DeviceCheckState::TimedOut; return; }
        if (snapshot.status != DeviceStatus::Ready || !snapshot.notificationsAvailable) {
            state_ = DeviceCheckState::EnumerationFailed; return;
        }
        if (snapshot.generation <= generation_) return;
        generation_ = snapshot.generation;
        if (snapshot.notificationCount < lastCount_) { state_ = DeviceCheckState::NotificationMissing; return; }
        lastCount_ = snapshot.notificationCount;
        const auto* endpoint = findEndpoint(snapshot, id_, DeviceFlow::Capture);
        const bool disconnected = !endpoint || endpoint->state == DeviceState::Unplugged || endpoint->state == DeviceState::NotPresent;
        const bool reconnected = endpoint && endpoint->state == DeviceState::Active;
        if ((state_ == DeviceCheckState::AwaitDisconnect && disconnected) ||
            (state_ == DeviceCheckState::AwaitReconnect && reconnected)) {
            if (snapshot.notificationCount == count_) { state_ = DeviceCheckState::NotificationMissing; return; }
            state_ = state_ == DeviceCheckState::AwaitDisconnect ? DeviceCheckState::AwaitReconnect : DeviceCheckState::Observed;
            count_ = snapshot.notificationCount;
        }
    }
    void cancel() noexcept { state_ = DeviceCheckState::Idle; }
    [[nodiscard]] bool running() const noexcept {
        return state_ == DeviceCheckState::AwaitDisconnect || state_ == DeviceCheckState::AwaitReconnect;
    }
    [[nodiscard]] DeviceCheckState state() const noexcept { return state_; }
private:
    DeviceCheckState state_ = DeviceCheckState::Idle;
    std::string id_;
    std::uint64_t generation_ = 0;
    std::uint64_t count_ = 0;
    std::uint64_t lastCount_ = 0;
    double deadline_ = 0;
};
}
