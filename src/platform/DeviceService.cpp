#include "platform/DeviceService.h"
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <utility>

namespace shirane {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
// GetProcessId documentation defines this success code; SDK 26100 omits its name.
constexpr HRESULT kNoSingleProcess = AUDCLNT_SUCCESS(0x00d);
struct CoString {
    LPWSTR value = nullptr;
    ~CoString() { CoTaskMemFree(value); }
};
struct PropertyValue {
    PROPVARIANT value{};
    ~PropertyValue() { const auto result = PropVariantClear(&value); if (FAILED(result)) OutputDebugStringW(L"Shirane: PropVariantClear failed\n"); }
};
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct HandleCloser {
    void operator()(void* handle) const noexcept { if (handle && !CloseHandle(handle)) OutputDebugStringW(L"Shirane: CloseHandle failed\n"); }
};
std::string utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("UTF-16 conversion failed");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result.data(), size, nullptr, nullptr))
        throw std::runtime_error("UTF-16 conversion failed");
    result.pop_back();
    return result;
}
bool checked(HRESULT result, const char* operation, DeviceSnapshot& snapshot) {
    if (SUCCEEDED(result)) return true;
    snapshot.status = DeviceStatus::Partial;
    if (!snapshot.nativeError) { snapshot.nativeError = result; snapshot.operation = operation; }
    return false;
}
std::string processName(DWORD pid) {
    std::unique_ptr<void, HandleCloser> process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return {}; // Protected/exited process: retain the PID label, never request elevation.
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) return {};
    path.resize(length);
    const auto separator = path.find_last_of(L"\\/");
    return utf8(path.c_str() + (separator == std::wstring::npos ? 0 : separator + 1));
}
class Notifications final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMNotificationClient> {
public:
    std::atomic<std::uint64_t> count{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    // Endpoint callbacks only coalesce a notification counter. No COM, locks or UI.
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return signal(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return signal(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return signal(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return signal(); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return signal(); }
private:
    HRESULT signal() noexcept { count.fetch_add(1, std::memory_order_relaxed); return S_OK; }
};
struct Registration {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<Notifications> listener = Microsoft::WRL::Make<Notifications>();
    bool registered = false;
    HRESULT notificationResult = E_PENDING;
    ~Registration() {
        if (registered) {
            const auto result = enumerator->UnregisterEndpointNotificationCallback(listener.Get());
            if (FAILED(result)) {
                // MMDevice does not own a reference. Keep this self-contained callback alive
                // if unregistration fails, rather than exposing a dangling callback pointer.
                OutputDebugStringW(L"Shirane: notification unregistration failed; callback retained\n");
                (void)listener.Detach();
            }
        }
    }
    HRESULT initialize() {
        if (!listener) return E_OUTOFMEMORY;
        const auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) return result;
        notificationResult = enumerator->RegisterEndpointNotificationCallback(listener.Get());
        registered = SUCCEEDED(notificationResult);
        return S_OK;
    }
};
DeviceState deviceState(DWORD value) noexcept {
    if (value & DEVICE_STATE_ACTIVE) return DeviceState::Active;
    if (value & DEVICE_STATE_DISABLED) return DeviceState::Disabled;
    if (value & DEVICE_STATE_UNPLUGGED) return DeviceState::Unplugged;
    return DeviceState::NotPresent;
}
void readSessions(IMMDevice* device, const std::string& endpointId, DeviceSnapshot& snapshot) {
    ComPtr<IAudioSessionManager2> manager;
    if (!checked(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(manager.GetAddressOf())), "Activate(SessionManager)", snapshot)) return;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (!checked(manager->GetSessionEnumerator(&sessions), "GetSessionEnumerator", snapshot)) return;
    int count = 0;
    if (!checked(sessions->GetCount(&count), "Session GetCount", snapshot)) return;
    constexpr int kMaxSessions = 256;
    if (count > kMaxSessions) checked(HRESULT_FROM_WIN32(ERROR_MORE_DATA), "Session limit", snapshot);
    for (int i = 0; i < std::min(count, kMaxSessions); ++i) {
        ComPtr<IAudioSessionControl> control;
        ComPtr<IAudioSessionControl2> details;
        if (!checked(sessions->GetSession(i, &control), "GetSession", snapshot) ||
            !checked(control.As(&details), "SessionControl2", snapshot)) continue;
        SessionInfo session;
        session.endpointId = endpointId;
        CoString instance;
        if (!checked(details->GetSessionInstanceIdentifier(&instance.value), "GetSessionInstanceIdentifier", snapshot)) continue;
        session.instanceId = utf8(instance.value);
        DWORD pid = 0;
        const auto pidResult = details->GetProcessId(&pid);
        if (!checked(pidResult, "GetProcessId", snapshot)) continue;
        session.processId = pid;
        session.multipleProcesses = pidResult == kNoSingleProcess;
        AudioSessionState state{};
        if (!checked(control->GetState(&state), "Session GetState", snapshot)) continue;
        if (state == AudioSessionStateExpired) continue;
        session.active = state == AudioSessionStateActive;
        CoString name;
        if (checked(control->GetDisplayName(&name.value), "GetDisplayName", snapshot)) session.name = utf8(name.value);
        const auto system = details->IsSystemSoundsSession();
        checked(system, "IsSystemSoundsSession", snapshot);
        if (system == S_OK) session.name = "システム サウンド";
        else if (!session.name.empty() && session.name.front() == '@') session.name.clear();
        if (session.name.empty() && !session.multipleProcesses) session.name = processName(pid);
        if (session.name.empty()) session.name = "アプリ PID " + std::to_string(pid);
        ComPtr<ISimpleAudioVolume> volume;
        if (checked(control.As(&volume), "ISimpleAudioVolume", snapshot)) {
            BOOL mute = FALSE;
            const bool gotVolume = checked(volume->GetMasterVolume(&session.volume), "GetMasterVolume", snapshot);
            const bool gotMute = checked(volume->GetMute(&mute), "GetMute", snapshot);
            session.volumeAvailable = gotVolume && gotMute;
            session.mute = mute != FALSE;
        }
        snapshot.sessions.push_back(std::move(session));
    }
}
DeviceSnapshot enumerate(Registration& registration) {
    DeviceSnapshot snapshot;
    snapshot.status = DeviceStatus::Ready;
    snapshot.notificationsAvailable = registration.registered;
    checked(registration.notificationResult, "RegisterEndpointNotificationCallback", snapshot);
    for (const auto flow : {eCapture, eRender}) {
        std::string defaultId;
        ComPtr<IMMDevice> defaultDevice;
        const auto defaultResult = registration.enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &defaultDevice);
        if (SUCCEEDED(defaultResult)) {
            CoString id;
            if (checked(defaultDevice->GetId(&id.value), "Default GetId", snapshot)) defaultId = utf8(id.value);
        } else if (defaultResult != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) checked(defaultResult, "GetDefaultAudioEndpoint", snapshot);
        ComPtr<IMMDeviceCollection> collection;
        if (!checked(registration.enumerator->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, &collection), "EnumAudioEndpoints", snapshot)) continue;
        UINT count = 0;
        if (!checked(collection->GetCount(&count), "Device GetCount", snapshot)) continue;
        constexpr UINT kMaxEndpoints = 128;
        if (count > kMaxEndpoints) checked(HRESULT_FROM_WIN32(ERROR_MORE_DATA), "Endpoint limit", snapshot);
        for (UINT i = 0; i < std::min(count, kMaxEndpoints); ++i) {
            ComPtr<IMMDevice> device;
            if (!checked(collection->Item(i, &device), "Device Item", snapshot)) continue;
            CoString id;
            DWORD state = 0;
            if (!checked(device->GetId(&id.value), "GetId", snapshot) || !checked(device->GetState(&state), "Device GetState", snapshot)) continue;
            EndpointInfo endpoint;
            endpoint.id = utf8(id.value);
            endpoint.name = endpoint.id;
            endpoint.flow = flow == eCapture ? DeviceFlow::Capture : DeviceFlow::Render;
            endpoint.state = deviceState(state);
            endpoint.multimediaDefault = endpoint.id == defaultId;
            ComPtr<IPropertyStore> properties;
            if (checked(device->OpenPropertyStore(STGM_READ, &properties), "OpenPropertyStore", snapshot)) {
                PropertyValue name;
                if (checked(properties->GetValue(PKEY_Device_FriendlyName, &name.value), "FriendlyName", snapshot) &&
                    name.value.vt == VT_LPWSTR && name.value.pwszVal) endpoint.name = utf8(name.value.pwszVal);
            }
            if (flow == eRender && endpoint.state == DeviceState::Active) readSessions(device.Get(), endpoint.id, snapshot);
            snapshot.endpoints.push_back(std::move(endpoint));
        }
    }
    std::sort(snapshot.endpoints.begin(), snapshot.endpoints.end(), [](const auto& a, const auto& b) {
        if (a.flow != b.flow) return a.flow < b.flow;
        if (a.name != b.name) return a.name < b.name;
        return a.id < b.id;
    });
    return snapshot;
}
}
struct DeviceService::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool refresh = true;
    DeviceSnapshot latest;
    std::thread worker;
    Impl() : worker([this] { run(); }) {}
    ~Impl() {
        { std::lock_guard lock(mutex); stop = true; }
        wake.notify_one();
        worker.join();
    }
    void publish(DeviceSnapshot next) {
        std::lock_guard lock(mutex);
        next.generation = latest.generation + 1;
        latest = std::move(next);
    }
    void run() noexcept {
        try {
            ComApartment apartment;
            Registration registration;
            auto result = apartment.result;
            if (SUCCEEDED(result)) result = registration.initialize();
            if (FAILED(result)) {
                DeviceSnapshot failure;
                failure.status = DeviceStatus::Failed; failure.nativeError = result; failure.operation = "Device service initialization";
                publish(std::move(failure)); return;
            }
            std::uint64_t seen = 0;
            auto deadline = Clock::now();
            while (true) {
                bool requested;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return stop || refresh; });
                    if (stop) break;
                    requested = std::exchange(refresh, false);
                }
                const auto count = registration.listener->count.load(std::memory_order_relaxed);
                if (!requested && count == seen && Clock::now() < deadline) continue;
                auto next = enumerate(registration);
                next.notificationCount = count;
                publish(std::move(next));
                seen = count;
                // New manager/enumerator each sweep: point-in-time sessions, not retained
                // session COM pointers or a claim of lossless session event tracking.
                deadline = Clock::now() + std::chrono::seconds(2);
            }
        } catch (...) {
            try {
                DeviceSnapshot failure;
                failure.status = DeviceStatus::Failed; failure.nativeError = E_FAIL;
                failure.operation = "Device worker exception";
                publish(std::move(failure));
            } catch (...) { OutputDebugStringW(L"Shirane: device worker could not publish failure\n"); }
        }
    }
};
DeviceService::DeviceService() : impl_(std::make_unique<Impl>()) {}
DeviceService::~DeviceService() = default;
void DeviceService::requestRefresh() {
    { std::lock_guard lock(impl_->mutex); impl_->refresh = true; }
    impl_->wake.notify_one();
}
bool DeviceService::copyLatest(DeviceSnapshot& destination) {
    std::lock_guard lock(impl_->mutex);
    if (destination.generation == impl_->latest.generation) return false;
    destination = impl_->latest;
    return true;
}
}
