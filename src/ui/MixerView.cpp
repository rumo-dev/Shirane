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
    ImGui::TextColored(kAmber, "PHASE 0  /  音声未接続");
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
    ImGui::TextWrapped("出力デバイス未接続");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##monitorMaster", &profile.monitorMasterDb, -60, 6, "%+.1f dB", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored({0.62f, 0.68f, 1, 1}, "Stream / 配信に届ける音");
    ImGui::TextWrapped("送出先未接続 · OBS連携は未実装");
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
    caption("WASAPI / 未実装");
    ImGui::TextWrapped("Phase 1でデバイス列挙、Phase 3でマイク入出力、Phase 4でアプリ別取得を実装します。");
    ImGui::End();
    ImGui::Begin("診断 / DIAGNOSTICS");
    ImGui::TextUnformatted("Engine: 未実装 / 停止中\n内部形式（設計）: 48,000 Hz · planar float32\n実周期 / 実測遅延 / 欠落: 未測定\n音声スレッド: なし / RT経路: なし");
    ImGui::TextWrapped("設定とDocking配置はJSONへ保存します。起動時の音声開始や自動ミュート解除は行いません。");
    ImGui::End();
}
}
