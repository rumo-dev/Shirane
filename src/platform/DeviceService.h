#pragma once
#include "model/Devices.h"
#include <memory>

namespace shirane {
// Non-RT service. COM objects never leave its MTA worker.
class DeviceService {
public:
    DeviceService();
    ~DeviceService();
    DeviceService(const DeviceService&) = delete;
    DeviceService& operator=(const DeviceService&) = delete;
    void requestRefresh();
    // Copies only when a new generation exists; UI never reads mutable worker data.
    bool copyLatest(DeviceSnapshot& destination);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
