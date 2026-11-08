#include "platform/DeviceService.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <unordered_set>

// Explicit real-machine test. Never registered with ordinary CI unit tests.
int main() {
    using namespace std::chrono_literals;
    shirane::DeviceService service;
    shirane::DeviceSnapshot snapshot;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    std::uint64_t first = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (service.copyLatest(snapshot)) {
            if (snapshot.status == shirane::DeviceStatus::Failed || snapshot.status == shirane::DeviceStatus::Partial) {
                std::cerr << "Enumeration error: " << snapshot.operation << " HRESULT=" << std::hex << snapshot.nativeError << '\n';
                return 1;
            }
            if (!first) { first = snapshot.generation; service.requestRefresh(); }
            else if (snapshot.generation > first) {
                std::unordered_set<std::string> ids;
                unsigned active = 0;
                for (const auto& endpoint : snapshot.endpoints) {
                    if (endpoint.id.empty() || !ids.insert(endpoint.id).second) return 2;
                    if (endpoint.state == shirane::DeviceState::Active) ++active;
                }
                std::cout << "Endpoints=" << snapshot.endpoints.size() << " active=" << active
                    << " sessions=" << snapshot.sessions.size() << " notificationsRegistered=" << snapshot.notificationsAvailable
                    << " generations=" << snapshot.generation << '\n';
                if (!active) { std::cout << "SKIP: no active audio hardware\n"; return 77; }
                return snapshot.notificationsAvailable ? 0 : 3;
            }
        }
        std::this_thread::sleep_for(50ms);
    }
    std::cerr << "Timed out waiting for device snapshots\n";
    return 4;
}
