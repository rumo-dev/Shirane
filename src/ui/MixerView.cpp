#include "ui/MixerView.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <algorithm>

#ifndef IMGUI_HAS_DOCK
#error Shirane requires Dear ImGui Docking
#endif

namespace shirane {
namespace {
const ImVec4 kMint{0.34f, 0.86f, 0.72f, 1.0f};
const ImVec4 kMuted{0.49f, 0.56f, 0.62f, 1.0f};
const ImVec4 kAmber{0.96f, 0.73f, 0.40f, 1.0f};
void caption(const char* text) { ImGui::TextColored(kMuted, "%s", text); }
const char* stateName(DeviceState state) {
    switch (state) {
    case DeviceState::Active: return "利用可能";
    case DeviceState::Disabled: return "無効";
    case DeviceState::Unplugged: return "未接続";
    default: return "見つかりません";
    }
}
void deviceSelector(const char* label, std::string& id, DeviceFlow flow, const DeviceSnapshot& snapshot, const std::string& excluded = {}) {
    const auto* selected = findEndpoint(snapshot, id, flow);
    const auto preview = id.empty() ? std::string("未選択") : selected ? selected->name : "見つかりません（選択IDを保持）";
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##device", preview.c_str())) {
        if (ImGui::Selectable("未選択", id.empty())) id.clear();
        for (const auto& endpoint : snapshot.endpoints) {
            if (endpoint.flow != flow) continue;
            ImGui::PushID(endpoint.id.c_str());
            const auto text = endpoint.name + (endpoint.multimediaDefault ? " [既定]" : "") + " / " + stateName(endpoint.state);
            ImGui::BeginDisabled(endpoint.state != DeviceState::Active || endpoint.id == excluded);
            if (ImGui::Selectable(text.c_str(), endpoint.id == id)) id = endpoint.id;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", endpoint.id.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (selected && selected->state != DeviceState::Active) ImGui::TextColored(kAmber, "%s · 選択を保持", stateName(selected->state));
    if (!id.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", id.c_str());
    ImGui::PopID();
}
void drawDevices(Profile& profile, ViewState& view) {
    auto& snapshot = view.devices;
    if (ImGui::Button("一覧を更新")) view.refreshDevicesRequested = true;
    ImGui::SameLine(); caption("デバイス変更通知 + 2秒ごとの再列挙 / OS音量は読取専用");
    if (snapshot.status == DeviceStatus::Loading) ImGui::TextUnformatted("デバイスを確認しています...");
    if (snapshot.status == DeviceStatus::Failed || snapshot.status == DeviceStatus::Partial)
        ImGui::TextColored(kAmber, "%s / %s / 0x%08X", snapshot.status == DeviceStatus::Failed ? "列挙失敗（再起動が必要）" : "一部情報を取得できません",
            snapshot.operation.c_str(), static_cast<unsigned>(snapshot.nativeError));
    if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); deviceSelector("マイク入力", profile.devices.microphoneId, DeviceFlow::Capture, snapshot);
        ImGui::TableNextColumn(); deviceSelector("Monitor出力", profile.devices.monitorId, DeviceFlow::Render, snapshot, profile.devices.streamId);
        ImGui::TableNextColumn(); deviceSelector("Stream送出先", profile.devices.streamId, DeviceFlow::Render, snapshot, profile.devices.monitorId);
        ImGui::EndTable();
    }
    caption("選択は保存可能な希望設定です。音声接続・Windowsの既定変更は行いません。");
    const auto* systemMic = defaultCapture(snapshot);
    ImGui::TextWrapped("Windows既定の入力: %s", systemMic ? systemMic->name.c_str() : "取得できません");
    if (ImGui::CollapsingHeader("既定の入力変更を確認")) {
        ImGui::TextWrapped("Shiraneのマイク選択を固定したまま確認開始。Windowsの既定入力を別の機器へ変更し、変更観測後に元へ戻してください。制限時間180秒。");
        const auto* selected = findEndpoint(snapshot, profile.devices.microphoneId, DeviceFlow::Capture);
        ImGui::BeginDisabled(view.defaultCheck.running() || !systemMic || !selected || selected->state != DeviceState::Active ||
            snapshot.status != DeviceStatus::Ready || !snapshot.notificationsAvailable);
        if (ImGui::Button("既定変更の確認開始")) (void)view.defaultCheck.start(snapshot, profile.devices.microphoneId, ImGui::GetTime());
        ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("既定変更の確認をリセット")) view.defaultCheck.cancel();
        const char* result = "未実施";
        switch (view.defaultCheck.state()) {
        case DefaultCheckState::AwaitChange: result = "Windows既定入力の変更待ち"; break;
        case DefaultCheckState::AwaitRestore: result = "既定変更を観測: Windowsの入力を元のデバイスへ戻してください"; break;
        case DefaultCheckState::Observed: result = "既定変更と復帰を観測: Shiraneの選択ID維持・通知件数の増加あり"; break;
        case DefaultCheckState::Invalid: result = "未合格: 選択変更・取得不完全・通知不足を検出"; break;
        case DefaultCheckState::TimedOut: result = "未合格: 180秒以内に元の既定入力へ戻ったことを確認できません"; break;
        default: break;
        }
        ImGui::TextWrapped("%s", result);
        caption("multimedia役割の既定入力を確認します。通知件数は全デバイス合計です。");
    }
    if (ImGui::CollapsingHeader("マイクの抜き差し確認")) {
        ImGui::TextWrapped("取り外してよいUSBマイクを選び、確認開始後に抜いてください。切断表示を確認してから同じポートへ戻してください。制限時間は120秒です。");
        const auto* mic = findEndpoint(snapshot, profile.devices.microphoneId, DeviceFlow::Capture);
        ImGui::BeginDisabled(view.deviceCheck.running() || snapshot.status != DeviceStatus::Ready ||
            !snapshot.notificationsAvailable || !mic || mic->state != DeviceState::Active);
        if (ImGui::Button("確認開始")) {
            (void)view.deviceCheck.start(snapshot, profile.devices.microphoneId, ImGui::GetTime());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("確認をリセット")) view.deviceCheck.cancel();
        const char* result = "未実施";
        switch (view.deviceCheck.state()) {
        case DeviceCheckState::AwaitDisconnect: result = "切断待ち: 選択したマイクを抜いてください"; break;
        case DeviceCheckState::AwaitReconnect: result = "切断を観測: 同じマイクを同じポートへ戻してください"; break;
        case DeviceCheckState::Observed: result = "状態復帰を観測: 選択ID維持・通知件数の増加あり"; break;
        case DeviceCheckState::SelectionChanged: result = "未合格: 確認中にマイク選択が変わりました"; break;
        case DeviceCheckState::EnumerationFailed: result = "未合格: 一覧取得または通知登録が不完全です"; break;
        case DeviceCheckState::NotificationMissing: result = "未合格: 状態変化に対応する通知件数増加を確認できません"; break;
        case DeviceCheckState::TimedOut: result = "未合格: 120秒以内に往復を確認できませんでした"; break;
        default: break;
        }
        ImGui::TextWrapped("%s", result);
        caption("通知件数は全デバイスの合計です。他デバイスの操作を重ねず確認してください。音声復旧の試験ではありません。");
    }
    if (ImGui::CollapsingHeader("音声デバイス一覧")) {
        for (const auto& endpoint : snapshot.endpoints) {
            ImGui::BulletText("%s | %s | %s%s", endpoint.flow == DeviceFlow::Capture ? "入力" : "出力",
                endpoint.name.c_str(), stateName(endpoint.state), endpoint.multimediaDefault ? " [既定: multimedia]" : "");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", endpoint.id.c_str());
        }
    }
    if (ImGui::CollapsingHeader("アプリ別OSセッション（読取専用）", ImGuiTreeNodeFlags_DefaultOpen)) {
        caption("Windowsの音量です。Shirane内部のフェーダーとは別です。短時間のセッションを見逃す場合があります。");
        if (ImGui::BeginTable("sessions", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("アプリ / PID"); ImGui::TableSetupColumn("再生デバイス");
            ImGui::TableSetupColumn("状態"); ImGui::TableSetupColumn("OS音量 / Mute"); ImGui::TableHeadersRow();
            for (const auto& session : snapshot.sessions) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Text("%s (%u)%s", session.name.c_str(), session.processId, session.multipleProcesses ? " [複数プロセス]" : "");
                ImGui::TableNextColumn();
                const auto* endpoint = findEndpoint(snapshot, session.endpointId, DeviceFlow::Render);
                ImGui::TextUnformatted(endpoint ? endpoint->name.c_str() : "不明");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(session.active ? "アクティブ" : "待機");
                ImGui::TableNextColumn();
                if (session.volumeAvailable) ImGui::Text("%.0f%% / %s", session.volume * 100, session.mute ? "MUTE" : "ON");
                else ImGui::TextUnformatted("取得不可");
            }
            ImGui::EndTable();
        }
        if (snapshot.sessions.empty()) caption("取得できたセッションはありません。");
    }
}
void sendControls(const char* id, const char* title, SendSettings& send, ImVec4 color) {
    ImGui::PushID(id);
    ImGui::Spacing();
    ImGui::TextColored(color, "%s", title);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##gain", &send.gainDb, -60.0f, 6.0f, "%+.1f dB", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##pan", &send.pan, -1.0f, 1.0f, "PAN %+.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("Mute", &send.mute);
    ImGui::SameLine();
    ImGui::Checkbox("Solo", &send.solo);
    ImGui::PopID();
}
void initialLayout(ImGuiID dock, const ImVec2& size) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, size);
    auto center = dock;
    const auto right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    const auto bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
    ImGui::DockBuilderDockWindow("ミキサー / MIXER", center);
    ImGui::DockBuilderDockWindow("出力 / OUTPUT", right);
    ImGui::DockBuilderDockWindow("ルーティング / ROUTING", bottom);
    ImGui::DockBuilderDockWindow("エフェクト / EFFECTS", bottom);
    ImGui::DockBuilderDockWindow("デバイス / DEVICES", bottom);
    ImGui::DockBuilderDockWindow("診断 / DIAGNOSTICS", bottom);
    ImGui::DockBuilderFinish(dock);
}
}
void applyTheme() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {18, 16};
    style.FramePadding = {8, 5};
    style.ItemSpacing = {8, 8};
    style.WindowRounding = 0;
    style.ChildRounding = 8;
    style.FrameRounding = 5;
    style.GrabRounding = 4;
    style.TabRounding = 4;
    style.WindowBorderSize = 0;
    style.Colors[ImGuiCol_WindowBg] = {0.055f, 0.073f, 0.09f, 1};
    style.Colors[ImGuiCol_ChildBg] = {0.08f, 0.105f, 0.125f, 1};
    style.Colors[ImGuiCol_PopupBg] = {0.08f, 0.105f, 0.125f, 1};
    style.Colors[ImGuiCol_Text] = {0.88f, 0.92f, 0.94f, 1};
    style.Colors[ImGuiCol_TextDisabled] = kMuted;
    style.Colors[ImGuiCol_FrameBg] = {0.13f, 0.18f, 0.21f, 1};
    style.Colors[ImGuiCol_Button] = {0.13f, 0.22f, 0.24f, 1};
    style.Colors[ImGuiCol_ButtonHovered] = {0.18f, 0.35f, 0.35f, 1};
    style.Colors[ImGuiCol_ButtonActive] = {0.22f, 0.43f, 0.40f, 1};
    style.Colors[ImGuiCol_CheckMark] = kMint;
    style.Colors[ImGuiCol_SliderGrab] = kMint;
    style.Colors[ImGuiCol_SliderGrabActive] = {0.55f, 0.98f, 0.85f, 1};
    style.Colors[ImGuiCol_Header] = {0.12f, 0.24f, 0.26f, 1};
    style.Colors[ImGuiCol_Tab] = {0.08f, 0.12f, 0.15f, 1};
    style.Colors[ImGuiCol_TabSelected] = {0.15f, 0.26f, 0.28f, 1};
    style.Colors[ImGuiCol_DockingPreview] = {0.34f, 0.86f, 0.72f, 0.4f};
}
void drawStudio(Profile& profile, ViewState& state) {
    state.deviceCheck.observe(state.devices, profile.devices.microphoneId, ImGui::GetTime());
    state.defaultCheck.observe(state.devices, profile.devices.microphoneId, ImGui::GetTime());
    auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("Shirane Studio", nullptr, flags);
    ImGui::TextColored(kMint, "調音  /  SHIRANE");
    ImGui::SameLine(); caption("     STREAM AUDIO STUDIO");
    ImGui::SameLine(ImGui::GetWindowWidth() - 440);
    if (ImGui::Button("設定を保存")) state.saveRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("再読込")) state.loadRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("配置を戻す")) state.resetLayout = true;
    ImGui::Separator();
    ImGui::TextColored(kAmber, "PHASE 1  /  音声未接続");
    ImGui::SameLine(); caption("入力と2つのバスを、ひとつの場所で。");
    const auto dock = ImGui::GetID("StudioDock");
    if (!ImGui::DockBuilderGetNode(dock) || state.resetLayout) {
        initialLayout(dock, ImGui::GetContentRegionAvail());
        state.resetLayout = false;
    }
    ImGui::DockSpace(dock, {0, -34});
    caption(state.notice.c_str());
    ImGui::End();

    ImGui::Begin("ミキサー / MIXER");
    caption("CHANNELS  /  希望設定 · 音声には未適用");
    if (ImGui::BeginTable("strips", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollX, {0, 0},
        std::max(950.0f, ImGui::GetContentRegionAvail().x))) {
        constexpr std::array<const char*, 5> numbers{"01 / INPUT", "02 / APP", "03 / APP", "04 / APP", "05 / APP"};
        for (std::size_t i = 0; i < profile.channels.size(); ++i) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 12});
            ImGui::BeginChild("strip", {0, 0}, ImGuiChildFlags_Borders);
            auto& channel = profile.channels[i];
            caption(numbers[i]);
            ImGui::TextUnformatted(channel.name.c_str());
            ImGui::TextColored(kMuted, "○ 未接続");
            ImGui::Separator();
            // No synthetic signal: meters stay empty until a real engine exists.
            ImGui::ProgressBar(0, {-1, 8}, "");
            caption("— dBFS");
            sendControls("monitor", "MONITOR", channel.monitor, kMint);
            ImGui::Separator();
            sendControls("stream", "STREAM", channel.stream, {0.62f, 0.68f, 1, 1});
            ImGui::Spacing();
            ImGui::TextWrapped("%s", i == 0 ? "マイクのモニターは初期OFF" : "元アプリの直接出力は別管理");
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();

    ImGui::Begin("出力 / OUTPUT");
    caption("MASTER BUSES");
    ImGui::TextColored(kMint, "Monitor / 自分に聞こえる音");
    const auto* monitor = findEndpoint(state.devices, profile.devices.monitorId, DeviceFlow::Render);
    ImGui::TextWrapped("選択: %s / 音声未接続", monitor ? monitor->name.c_str() : profile.devices.monitorId.empty() ? "未選択" : "見つかりません");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##monitorMaster", &profile.monitorMasterDb, -60, 6, "%+.1f dB", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored({0.62f, 0.68f, 1, 1}, "Stream / 配信に届ける音");
    const auto* stream = findEndpoint(state.devices, profile.devices.streamId, DeviceFlow::Render);
    ImGui::TextWrapped("選択: %s / OBS送出未実装", stream ? stream->name.c_str() : profile.devices.streamId.empty() ? "未選択" : "見つかりません");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##streamMaster", &profile.streamMasterDb, -60, 6, "%+.1f dB", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(kAmber, "安全な起動 / 音声停止中");
    ImGui::BeginDisabled();
    ImGui::Button("音声開始（未実装）", {-1, 42});
    ImGui::EndDisabled();
    if (ImGui::Button("すべての送信設定をミュート", {-1, 42})) {
        for (auto& channel : profile.channels) { channel.monitor.mute = true; channel.stream.mute = true; }
        state.notice = "すべての希望設定をミュートしました（音声エンジン未接続）";
    }
    ImGui::TextWrapped("フェーダー、Mute、Solo、Panは保存可能な設定です。現在、音声の取得・再生は行いません。");
    ImGui::End();

    ImGui::Begin("ルーティング / ROUTING");
    caption("SOURCE → MONITOR / STREAM  ·  設定プレビュー");
    if (ImGui::BeginTable("routes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("入力"); ImGui::TableSetupColumn("Monitor send"); ImGui::TableSetupColumn("Stream send"); ImGui::TableHeadersRow();
        for (const auto& channel : profile.channels) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(channel.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s / %+.1f dB", channel.monitor.mute ? "MUTE" : "ON", channel.monitor.gainDb);
            ImGui::TableNextColumn(); ImGui::Text("%s / %+.1f dB", channel.stream.mute ? "MUTE" : "ON", channel.stream.gainDb);
        }
        ImGui::EndTable();
    }
    ImGui::End();
    ImGui::Begin("エフェクト / EFFECTS");
    caption("DSP / 未実装");
    ImGui::TextWrapped("EQ、Gate、Compressor、LimiterはPhase 6で追加予定です。現在の画面はDSP処理を行いません。");
    ImGui::End();
    ImGui::Begin("デバイス / DEVICES");
    drawDevices(profile, state);
    ImGui::End();
    ImGui::Begin("診断 / DIAGNOSTICS");
    ImGui::Text("デバイス更新: %llu / 通知: %llu / 通知登録: %s", static_cast<unsigned long long>(state.devices.generation),
        static_cast<unsigned long long>(state.devices.notificationCount), state.devices.notificationsAvailable ? "有効" : "未登録");
    ImGui::TextUnformatted("Engine: 未実装 / 停止中\n内部形式（設計）: 48,000 Hz · planar float32\n実周期 / 実測遅延 / 欠落: 未測定\n音声スレッド: なし / RT経路: なし");
    ImGui::TextWrapped("設定とDocking配置はJSONへ保存します。起動時の音声開始や自動ミュート解除は行いません。");
    ImGui::End();
}
}
