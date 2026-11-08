#pragma once
#include "model/Devices.h"

namespace shirane {
enum class DefaultCheckState { Idle, AwaitChange, AwaitRestore, Observed, Invalid, TimedOut };
inline const EndpointInfo* defaultCapture(const DeviceSnapshot& snapshot) noexcept {
    for (const auto& endpoint : snapshot.endpoints)
        if (endpoint.flow == DeviceFlow::Capture && endpoint.multimediaDefault && endpoint.state == DeviceState::Active) return &endpoint;
    return nullptr;
}
// Observes global notification counts and enumerated default changes. Does not change Windows settings.
class DefaultDeviceCheck {
public:
    bool start(const DeviceSnapshot& snapshot, const std::string& selectedId, double now) {
        const auto* selected = findEndpoint(snapshot, selectedId, DeviceFlow::Capture);
        const auto* current = defaultCapture(snapshot);
        if (snapshot.status != DeviceStatus::Ready || !snapshot.notificationsAvailable || !snapshot.generation ||
            !selected || selected->state != DeviceState::Active || !current) return false;
        selected_ = selectedId; original_ = current->id;
        count_ = snapshot.notificationCount; lastCount_ = count_; generation_ = snapshot.generation;
        deadline_ = now + 180; state_ = DefaultCheckState::AwaitChange;
        return true;
    }
    void observe(const DeviceSnapshot& snapshot, const std::string& selectedId, double now) noexcept {
        if (!running()) return;
        if (now >= deadline_) { state_ = DefaultCheckState::TimedOut; return; }
        const auto* selected = findEndpoint(snapshot, selected_, DeviceFlow::Capture);
        if (selectedId != selected_ || snapshot.status != DeviceStatus::Ready || !snapshot.notificationsAvailable ||
            !selected || selected->state != DeviceState::Active) { state_ = DefaultCheckState::Invalid; return; }
        if (snapshot.generation <= generation_) return;
        generation_ = snapshot.generation;
        if (snapshot.notificationCount < lastCount_) { state_ = DefaultCheckState::Invalid; return; }
        lastCount_ = snapshot.notificationCount;
        const auto* current = defaultCapture(snapshot);
        if (!current) { state_ = DefaultCheckState::Invalid; return; }
        if ((state_ == DefaultCheckState::AwaitChange && current->id != original_) ||
            (state_ == DefaultCheckState::AwaitRestore && current->id == original_)) {
            if (snapshot.notificationCount <= count_) { state_ = DefaultCheckState::Invalid; return; }
            count_ = snapshot.notificationCount;
            state_ = state_ == DefaultCheckState::AwaitChange ? DefaultCheckState::AwaitRestore : DefaultCheckState::Observed;
        }
    }
    void cancel() noexcept { state_ = DefaultCheckState::Idle; }
    bool running() const noexcept { return state_ == DefaultCheckState::AwaitChange || state_ == DefaultCheckState::AwaitRestore; }
    DefaultCheckState state() const noexcept { return state_; }
private:
    DefaultCheckState state_ = DefaultCheckState::Idle;
    std::string selected_, original_;
    std::uint64_t count_ = 0, lastCount_ = 0, generation_ = 0;
    double deadline_ = 0;
};
}
