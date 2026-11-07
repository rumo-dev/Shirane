#pragma once
#include "config/Profile.h"
namespace shirane {
struct ViewState {
    bool saveRequested = false;
    bool loadRequested = false;
    bool resetLayout = false;
    std::string notice = "音声エンジン未接続 • 設定のみ編集できます";
};
void applyTheme();
void drawStudio(Profile& profile, ViewState& state);
}
