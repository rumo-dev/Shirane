#include "ui/MixerView.h"
#include <Windows.h>
#include <ShlObj.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        spdlog::error("{} failed: HRESULT={:08X}", operation, static_cast<unsigned long>(result));
        throw std::runtime_error(operation);
    }
}
struct CoTaskDeleter { void operator()(wchar_t* pointer) const noexcept { CoTaskMemFree(pointer); } };
std::filesystem::path settingsDirectory() {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    std::unique_ptr<wchar_t, CoTaskDeleter> owner(raw);
    check(result, "SHGetKnownFolderPath");
    return std::filesystem::path(owner.get()) / L"Shirane";
}
class AppHost {
public:
    ~AppHost() {
        if (dxBackend_) ImGui_ImplDX11_Shutdown();
        if (winBackend_) ImGui_ImplWin32_Shutdown();
        if (imgui_) ImGui::DestroyContext();
        if (context_) context_->ClearState();
        target_.Reset(); swap_.Reset(); context_.Reset(); device_.Reset();
        if (window_) DestroyWindow(window_);
        if (windowClass_) UnregisterClassW(L"ShiraneStudio", GetModuleHandleW(nullptr));
    }
    int run(bool smoke) {
        // Automated startup checks must not read or change the user's profile/logs.
        const auto directory = smoke ? std::filesystem::current_path() / "out" / "smoke" : settingsDirectory();
        std::filesystem::create_directories(directory);
        spdlog::set_default_logger(spdlog::rotating_logger_mt("shirane", (directory / "shirane.log").string(), 1024 * 1024, 3));
        spdlog::flush_on(spdlog::level::info);
        spdlog::info("Shirane Phase 0 starting; audio unavailable");
        const auto profilePath = directory / L"profile.json";
        auto profile = shirane::defaultProfile();
        shirane::ViewState view;
        const auto loaded = shirane::loadProfile(profilePath, profile);
        if (!loaded.ok() && loaded.code != shirane::StoreCode::Missing) view.notice = loaded.message;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc); wc.style = CS_CLASSDC; wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ShiraneStudio";
        windowClass_ = RegisterClassExW(&wc);
        if (!windowClass_) throw std::runtime_error("RegisterClassExW");
        window_ = CreateWindowExW(0, wc.lpszClassName, L"調音 Shirane — Stream Audio Studio", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1440, 940, nullptr, nullptr, wc.hInstance, this);
        if (!window_) throw std::runtime_error("CreateWindowExW");
        createGraphics();
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); imgui_ = true;
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        io.IniFilename = nullptr;
        wchar_t windowsPath[MAX_PATH]{};
        if (!GetWindowsDirectoryW(windowsPath, MAX_PATH)) throw std::runtime_error("GetWindowsDirectoryW");
        const auto fontPath = std::filesystem::path(windowsPath) / "Fonts" / "meiryo.ttc";
        ImFontGlyphRangesBuilder rangesBuilder;
        rangesBuilder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        rangesBuilder.AddText("○—•→");
        rangesBuilder.BuildRanges(&glyphRanges_);
        if (!std::filesystem::exists(fontPath) || !io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 18.0f, nullptr, glyphRanges_.Data))
            throw std::runtime_error("Japanese font meiryo.ttc unavailable");
        shirane::applyTheme();
        if (!profile.layout.empty()) ImGui::LoadIniSettingsFromMemory(profile.layout.c_str(), profile.layout.size());
        if (!ImGui_ImplWin32_Init(window_)) throw std::runtime_error("ImGui Win32 initialization");
        winBackend_ = true;
        if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) throw std::runtime_error("ImGui DX11 initialization");
        dxBackend_ = true;
        ShowWindow(window_, SW_SHOWDEFAULT); UpdateWindow(window_);
        bool done = false;
        unsigned frames = 0;
        while (!done) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
                if (message.message == WM_QUIT) done = true;
            }
            if (done) break;
            if (IsIconic(window_)) { MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE); continue; }
            if (pendingWidth_ && pendingHeight_) {
                context_->OMSetRenderTargets(0, nullptr, nullptr);
                target_.Reset();
                check(swap_->ResizeBuffers(0, pendingWidth_, pendingHeight_, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
                pendingWidth_ = pendingHeight_ = 0;
                createTarget();
            }
            ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
            shirane::drawStudio(profile, view);
            if (view.saveRequested) {
                std::size_t size = 0;
                const char* layout = ImGui::SaveIniSettingsToMemory(&size);
                profile.layout.assign(layout, size);
                const auto result = shirane::saveProfile(profilePath, profile);
                view.notice = result.ok() ? "設定と画面配置を保存しました" : result.message;
                spdlog::info("Profile save code={}", static_cast<int>(result.code));
                view.saveRequested = false;
            }
            if (view.loadRequested) {
                const auto result = shirane::loadProfile(profilePath, profile);
                if (result.ok()) {
                    if (!profile.layout.empty()) ImGui::LoadIniSettingsFromMemory(profile.layout.c_str(), profile.layout.size());
                }
                view.notice = result.ok() ? "設定と画面配置を復元しました（音声停止中）" : result.message;
                view.loadRequested = false;
            }
            ImGui::Render();
            constexpr float clear[]{0.035f, 0.05f, 0.065f, 1};
            auto* target = target_.Get();
            context_->OMSetRenderTargets(1, &target, nullptr);
            context_->ClearRenderTargetView(target, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            const auto presented = swap_->Present(1, 0);
            if (presented == DXGI_STATUS_OCCLUDED) MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            else check(presented, "Present");
            ++frames;
            if (smoke && frames >= 20) done = true;
        }
        // Explicit save only: a corrupt/unsupported profile is never overwritten on exit.
        spdlog::info("Shirane clean shutdown; rendered frames={}", frames);
        return 0;
    }
private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
        auto* self = reinterpret_cast<AppHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<AppHost*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_SIZE && self && wparam != SIZE_MINIMIZED) {
            self->pendingWidth_ = LOWORD(lparam); self->pendingHeight_ = HIWORD(lparam); return 0;
        }
        if (message == WM_DPICHANGED) {
            const auto* rect = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (message == WM_GETMINMAXINFO) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {1100, 760}; return 0;
        }
        if (message == WM_CLOSE) { PostQuitMessage(0); return 0; }
        if (message == WM_SYSCOMMAND && (wparam & 0xfff0) == SC_KEYMENU) return 0;
        return DefWindowProcW(window, message, wparam, lparam);
    }
    void createGraphics() {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow = window_;
        desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        auto result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1,
            D3D11_SDK_VERSION, &desc, &swap_, &device_, nullptr, &context_);
        if (FAILED(result)) {
            spdlog::warn("Hardware DX11 unavailable: {:08X}; trying WARP", static_cast<unsigned long>(result));
            swap_.Reset(); context_.Reset(); device_.Reset();
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1,
                D3D11_SDK_VERSION, &desc, &swap_, &device_, nullptr, &context_);
        }
        check(result, "D3D11CreateDeviceAndSwapChain");
        createTarget();
    }
    void createTarget() {
        ComPtr<ID3D11Texture2D> buffer;
        check(swap_->GetBuffer(0, IID_PPV_ARGS(&buffer)), "GetBuffer");
        check(device_->CreateRenderTargetView(buffer.Get(), nullptr, &target_), "CreateRenderTargetView");
    }
    HWND window_ = nullptr;
    ATOM windowClass_ = 0;
    UINT pendingWidth_ = 0, pendingHeight_ = 0;
    bool imgui_ = false, winBackend_ = false, dxBackend_ = false;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> target_;
    ImVector<ImWchar> glyphRanges_;
};
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    const bool smoke = std::wstring(commandLine) == L"--smoke-test";
    try {
        AppHost host;
        return host.run(smoke);
    } catch (const std::exception& error) {
        spdlog::error("Application stopped: {}", error.what());
        if (!smoke) MessageBoxW(nullptr, L"Shiraneを起動・描画できませんでした。LocalAppData\\Shiraneのログを確認してください。", L"Shirane", MB_OK | MB_ICONERROR);
        return 1;
    }
}
