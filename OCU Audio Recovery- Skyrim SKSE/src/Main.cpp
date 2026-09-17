#include "PCH.h"

#include <RE/B/BSAudioManager.h>
#include <RE/B/BSSoundMessage.h>
#include <SKSE/SKSE.h>
#include <mmdeviceapi.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    using namespace std::chrono_literals;

    struct Settings
    {
        bool enabled{ false };
        bool debugLogging{ false };
        std::chrono::milliseconds reconnectDelay{ 2500 };
        std::chrono::milliseconds minimumRecoveryInterval{ 10000 };
    };

    Settings g_settings;
    std::filesystem::path g_iniPath;

    std::filesystem::path GetThisModulePath()
    {
        HMODULE module = nullptr;
        const auto address = reinterpret_cast<LPCWSTR>(&GetThisModulePath);
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                address,
                &module)) {
            return {};
        }

        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) {
            return {};
        }
        buffer.resize(length);
        return std::filesystem::path(buffer);
    }

    int ReadBoundedInt(const wchar_t* key, int defaultValue, int minimum, int maximum)
    {
        const int value = static_cast<int>(GetPrivateProfileIntW(
            L"AudioRecovery", key, defaultValue, g_iniPath.c_str()));
        return std::clamp(value, minimum, maximum);
    }

    void LoadSettings()
    {
        g_iniPath = GetThisModulePath();
        g_iniPath.replace_extension(L".ini");

        g_settings.enabled = ReadBoundedInt(L"bEnabled", 0, 0, 1) != 0;
        g_settings.debugLogging = ReadBoundedInt(L"bDebugLogging", 0, 0, 1) != 0;
        g_settings.reconnectDelay = std::chrono::milliseconds(
            ReadBoundedInt(L"iReconnectDelayMs", 2500, 500, 15000));
        g_settings.minimumRecoveryInterval = std::chrono::milliseconds(
            ReadBoundedInt(L"iMinimumRecoveryIntervalMs", 10000, 2000, 60000));
    }

    void RecoverSkyrimAudio()
    {
        // Safety rollback: a synchronous BSAudioManager reset can block forever
        // after XAudio loses its endpoint. Keep detection/logging available for
        // diagnostics, but never touch Skyrim's audio worker in this build.
        SKSE::log::warn("Playback endpoint recovered, but automatic Skyrim audio reset is disabled by the safety rollback");
    }

    class EndpointRecoveryService;

    class EndpointNotificationClient final : public IMMNotificationClient
    {
    public:
        explicit EndpointNotificationClient(EndpointRecoveryService& owner) : _owner(owner) {}

        ULONG STDMETHODCALLTYPE AddRef() override { return ++_references; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG remaining = --_references;
            if (remaining == 0) {
                delete this;
            }
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
        {
            if (!object) {
                return E_POINTER;
            }
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
                *object = static_cast<IMMNotificationClient*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override;
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override;
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR deviceId) override;
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    private:
        std::atomic<ULONG> _references{ 1 };
        EndpointRecoveryService& _owner;
    };

    class EndpointRecoveryService
    {
    public:
        void Start()
        {
            if (_started.exchange(true)) {
                return;
            }
            _thread = std::thread([this]() { Run(); });
            _thread.detach();
        }

        void NotifyDefaultChanged(ERole role, LPCWSTR deviceId)
        {
            if (role != eConsole && role != eMultimedia) {
                return;
            }
            {
                std::scoped_lock lock(_mutex);
                if (deviceId) {
                    _currentDefaultId = deviceId;
                } else {
                    _currentDefaultId.clear();
                }
            }
            RequestRecovery("default playback endpoint changed");
        }

        void NotifyDeviceState(LPCWSTR deviceId, DWORD state)
        {
            bool relevant = false;
            {
                std::scoped_lock lock(_mutex);
                relevant = deviceId && _currentDefaultId == deviceId;
            }
            if (!relevant) {
                return;
            }

            if ((state & DEVICE_STATE_ACTIVE) != 0) {
                RequestRecovery("default playback endpoint became active");
            } else {
                // Record the loss. The stable worker will wait for an active
                // default endpoint rather than resetting Skyrim into a void.
                RequestRecovery("default playback endpoint became unavailable");
            }
        }

        void NotifyDeviceRemoved(LPCWSTR deviceId)
        {
            NotifyDeviceState(deviceId, DEVICE_STATE_NOTPRESENT);
        }

    private:
        friend class EndpointNotificationClient;

        bool QueryActiveDefaultEndpoint(std::wstring& deviceId)
        {
            if (!_enumerator) {
                return false;
            }

            IMMDevice* device = nullptr;
            const HRESULT defaultResult = _enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (FAILED(defaultResult) || !device) {
                return false;
            }

            DWORD state = 0;
            LPWSTR id = nullptr;
            const HRESULT stateResult = device->GetState(&state);
            const HRESULT idResult = device->GetId(&id);
            if (SUCCEEDED(idResult) && id) {
                deviceId = id;
                CoTaskMemFree(id);
            }
            device->Release();
            return SUCCEEDED(stateResult) && (state & DEVICE_STATE_ACTIVE) != 0;
        }

        void RequestRecovery(const char* reason)
        {
            if (!_initialized.load()) {
                return;
            }
            {
                std::scoped_lock lock(_mutex);
                ++_generation;
                _lastSignal = std::chrono::steady_clock::now();
                _lastReason = reason;
            }
            if (g_settings.debugLogging) {
                SKSE::log::info("Audio endpoint event: {}", reason);
            }
            _condition.notify_one();
        }

        void Run()
        {
            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
                SKSE::log::error("Audio recovery failed to initialize COM: 0x{:08X}",
                    static_cast<std::uint32_t>(comResult));
                return;
            }

            const HRESULT createResult = CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&_enumerator));
            if (FAILED(createResult) || !_enumerator) {
                SKSE::log::error("Audio recovery failed to create MMDeviceEnumerator: 0x{:08X}",
                    static_cast<std::uint32_t>(createResult));
                if (SUCCEEDED(comResult)) {
                    CoUninitialize();
                }
                return;
            }

            std::wstring initialId;
            if (QueryActiveDefaultEndpoint(initialId)) {
                std::scoped_lock lock(_mutex);
                _currentDefaultId = initialId;
            }

            _callback = new EndpointNotificationClient(*this);
            const HRESULT registerResult = _enumerator->RegisterEndpointNotificationCallback(_callback);
            if (FAILED(registerResult)) {
                SKSE::log::error("Audio recovery failed to register endpoint notifications: 0x{:08X}",
                    static_cast<std::uint32_t>(registerResult));
                _callback->Release();
                _callback = nullptr;
                _enumerator->Release();
                _enumerator = nullptr;
                if (SUCCEEDED(comResult)) {
                    CoUninitialize();
                }
                return;
            }

            _initialized.store(true);
            SKSE::log::info("Windows audio endpoint watcher active (delay={} ms, minimum interval={} ms)",
                g_settings.reconnectDelay.count(), g_settings.minimumRecoveryInterval.count());

            std::uint64_t processedGeneration = 0;
            for (;;) {
                std::unique_lock lock(_mutex);
                _condition.wait(lock, [&]() { return _generation != processedGeneration; });

                std::uint64_t observedGeneration = _generation;
                auto due = _lastSignal + g_settings.reconnectDelay;
                if (_lastRecoveryQueued.time_since_epoch().count() != 0) {
                    due = (std::max)(due, _lastRecoveryQueued + g_settings.minimumRecoveryInterval);
                }

                while (_condition.wait_until(lock, due, [&]() { return _generation != observedGeneration; })) {
                    observedGeneration = _generation;
                    due = _lastSignal + g_settings.reconnectDelay;
                    if (_lastRecoveryQueued.time_since_epoch().count() != 0) {
                        due = (std::max)(due, _lastRecoveryQueued + g_settings.minimumRecoveryInterval);
                    }
                }

                const std::string reason = _lastReason;
                processedGeneration = observedGeneration;
                lock.unlock();

                std::wstring activeId;
                if (!QueryActiveDefaultEndpoint(activeId)) {
                    if (g_settings.debugLogging) {
                        SKSE::log::info("Audio recovery deferred: no active default playback endpoint");
                    }
                    continue;
                }

                {
                    std::scoped_lock stateLock(_mutex);
                    _currentDefaultId = std::move(activeId);
                    _lastRecoveryQueued = std::chrono::steady_clock::now();
                }

                SKSE::log::info("Stable playback endpoint detected after {}; scheduling Skyrim audio recovery", reason);
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() { RecoverSkyrimAudio(); });
                } else {
                    SKSE::log::error("Audio recovery could not access the SKSE task interface");
                }
            }
        }

        std::atomic_bool _started{ false };
        std::atomic_bool _initialized{ false };
        std::thread _thread;
        IMMDeviceEnumerator* _enumerator{ nullptr };
        EndpointNotificationClient* _callback{ nullptr };
        std::mutex _mutex;
        std::condition_variable _condition;
        std::wstring _currentDefaultId;
        std::string _lastReason;
        std::uint64_t _generation{ 0 };
        std::chrono::steady_clock::time_point _lastSignal{};
        std::chrono::steady_clock::time_point _lastRecoveryQueued{};
    };

    HRESULT EndpointNotificationClient::OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState)
    {
        _owner.NotifyDeviceState(deviceId, newState);
        return S_OK;
    }

    HRESULT EndpointNotificationClient::OnDeviceRemoved(LPCWSTR deviceId)
    {
        _owner.NotifyDeviceRemoved(deviceId);
        return S_OK;
    }

    HRESULT EndpointNotificationClient::OnDefaultDeviceChanged(
        EDataFlow flow, ERole role, LPCWSTR deviceId)
    {
        if (flow == eRender) {
            _owner.NotifyDefaultChanged(role, deviceId);
        }
        return S_OK;
    }

    EndpointRecoveryService& GetRecoveryService()
    {
        // SKSE plugins are never unloaded during normal play. Keep the watcher
        // alive until process termination so CRT teardown cannot destroy its
        // mutex/callback while the detached COM notification thread is waiting.
        static auto* service = new EndpointRecoveryService();
        return *service;
    }

    void SetupLogging()
    {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }
        *path /= "OCUAudioRecovery.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("OCUAudioRecovery", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
    }

    void OnSKSEMessage(SKSE::MessagingInterface::Message* message)
    {
        if (message->type == SKSE::MessagingInterface::kDataLoaded && g_settings.enabled) {
            GetRecoveryService().Start();
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SetupLogging();
    LoadSettings();

    SKSE::log::info("OCU Audio Recovery v0.2.1 loaded; enabled={}; INI={}",
        g_settings.enabled, g_iniPath.string());

    if (!REL::Module::IsVR()) {
        SKSE::log::critical("OCU Audio Recovery supports Skyrim VR only");
        return false;
    }

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
        SKSE::log::critical("Failed to register SKSE messaging listener");
        return false;
    }

    if (!g_settings.enabled) {
        SKSE::log::info("Audio recovery is disabled in OCUAudioRecovery.ini");
    }
    return true;
}
