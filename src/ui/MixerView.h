#pragma once
#include "config/Profile.h"
#include "model/DeviceCheck.h"
#include "model/DefaultDeviceCheck.h"
namespace shirane {
struct ViewState {
    bool saveRequested = false;
    bool loadRequested = false;
    bool resetLayout = false;
    bool refreshDevicesRequested = false;
    DeviceSnapshot devices;
    DeviceCheck deviceCheck;
    DefaultDeviceCheck defaultCheck;
    std::string notice = "音声エンジン未接続 • 設定のみ編集できます";
};
void applyTheme();
void drawStudio(Profile& profile, ViewState& state);
}
