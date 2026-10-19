#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_timer.h>

#include "ota.h"



#include <string>
#include <mutex>
#include <deque>

#include "audio/audio_service.h"
#include "device_state_machine.h"
#include "device_state.h"
#include "device_state_machine.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)



enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

enum ListeningMode {
    kListeningModeAutoStop,
    kListeningModeManualStop,
    kListeningModeRealtime // 需要 AEC 支持
};

class Application{
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();

    void Run();

    bool SetDeviceState(DeviceState new_state);
    void ActivationTask();
    void Schedule(std::function<void()>&& callback);
    void SendMCPMessage(const std::string& payload);
    AudioService& GetAudioService() { return audio_service_; }
    void Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound);
    DeviceState GetDeviceState() const { return device_state_machine_.GetCurrentState(); }
    void ResetProtocol();
private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    DeviceStateMachine device_state_machine_;
    AudioService audio_service_;
    EventGroupHandle_t event_group_ = nullptr;
    AecMode aec_mode_ = kAecOff;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    TaskHandle_t activation_task_handle_ = nullptr;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    std::unique_ptr<Ota> ota_;
    std::unique_ptr<Protocol> protocol_;
    std::string last_error_message_;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool has_server_time_ = false;
    int clock_ticks_ = 0;

   void HandleNetworkConnectedEvent();



    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    bool UpgradeFirmware(const std::string& url, const std::string& version);
    void HandleNetworkDisconnectedEvent();
    void InitializeProtocol();
    void HandleActivationDoneEvent();
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleWakeWordDetectedEvent();
    void Reboot();
};
