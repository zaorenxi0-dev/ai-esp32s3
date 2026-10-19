#include "application.h"
#include "boards/common/board.h"
#include "audio/audio_codec.h"
#include "device_state_machine.h"
#include "device_state.h"
#include "audio/audio_service.h"
#include "mcp_server.h"
#include "wifi_board.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "display/display.h"
#include "system_info.h"
#include "mqtt_protocol.h"

#include <esp_system.h>

#define TAG "Application"

Application::Application() {
    event_group_ = xEventGroupCreate();

 #if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
        clock_timer_handle_ = nullptr;
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool Application::SetDeviceState(DeviceState new_state) {
    return device_state_machine_.TransitionToState(new_state);
}

void Application::SendMCPMessage(const std::string& payload) {
    if (protocol_ != nullptr) {
        protocol_->SendMcpMessage(payload);
    }
}

void Application::Schedule(std::function<void()>&& callback){
    std::lock_guard<std::mutex> lock(mutex_);
    main_tasks_.push_back(std::move(callback));
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);   
}




void Application::Initialize(){
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);
    //获取显示屏实例


    //setup the audio service

    auto codec=board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_vad_change = [this](bool speaking){
        xEventGroupSetBits(event_group_,MAIN_EVENT_VAD_CHANGE);
    };

    callbacks.on_send_queue_available = [this](){
        xEventGroupSetBits(event_group_,MAIN_EVENT_SEND_AUDIO);
     };

    callbacks.on_wake_word_detected = [this](const std::string& wake_word){
        xEventGroupSetBits(event_group_,MAIN_EVENT_WAKE_WORD_DETECTED);
     };

    audio_service_.SetCallbacks(callbacks);

    device_state_machine_.AddStateCallback([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = MCPServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;
while(true){
    EventBits_t bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, false, true, portMAX_DELAY);

    if (bits & MAIN_EVENT_ERROR) {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, "Unknown error", "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
    }
    if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
        HandleNetworkConnectedEvent();
    }

    if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
        HandleNetworkDisconnectedEvent();
    }

    if (bits & MAIN_EVENT_ACTIVATION_DONE) {
        HandleActivationDoneEvent();
    }

    if (bits & MAIN_EVENT_STATE_CHANGED) {
        HandleStateChangedEvent();
    }

    if (bits & MAIN_EVENT_TOGGLE_CHAT) {
        HandleToggleChatEvent();
    }

    if (bits & MAIN_EVENT_SEND_AUDIO) {
        while (auto packet = audio_service_.PopPacketFromSendQueue()) {
            if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                break;
            }
        }
    }

    if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
        HandleWakeWordDetectedEvent();
    }


    if (bits & MAIN_EVENT_VAD_CHANGE) {
        if (GetDeviceState() == kDeviceStateListening) {

        }
    }


    if (bits & MAIN_EVENT_SCHEDULE) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto tasks = std::move(main_tasks_);
        lock.unlock();
        for (auto& task : tasks) {
            task();
        }
    }

    if (bits & MAIN_EVENT_CLOCK_TICK) {
        clock_ticks_++;

    
        // Print debug info every 10 seconds
        if (clock_ticks_ % 10 == 0) {
            SystemInfo::PrintHeapStats();
        }
    }

}
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    if (state == kDeviceStateIdle) {
        ESP_LOGI(TAG, "Wake word detected, opening audio channel");
        SetDeviceState(kDeviceStateConnecting);
        if (!protocol_->IsAudioChannelOpened()) {
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }
        // 关闭唤醒词、开启前处理与上行编码
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.EnableVoiceProcessing(true);
        SetDeviceState(kDeviceStateListening);
    } else if (state == kDeviceStateSpeaking) {
        // 说话中再次唤醒 —— 打断当前 TTS，直接进入监听
        ESP_LOGI(TAG, "Wake word detected while speaking, aborting and listening");
        aborted_ = true;
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.EnableVoiceProcessing(true);
        SetDeviceState(kDeviceStateListening);
    }
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    if (state == kDeviceStateIdle) {
        if (!protocol_) {
            return;
        }
        SetDeviceState(kDeviceStateConnecting);
        if (!protocol_->IsAudioChannelOpened()) {
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                return;
            }
        }
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.EnableVoiceProcessing(true);
        SetDeviceState(kDeviceStateListening);
    } else if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(true);
        SetDeviceState(kDeviceStateIdle);
    }
}







void Application::HandleStateChangedEvent() {

    ESP_LOGI(TAG, "State changed");

}



void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");
    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    // 进入待机后开启唤醒词检测，等待用户唤醒
    audio_service_.EnableWakeWordDetection(true);
}



void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();
    if(state == kDeviceStateStarting||state == kDeviceStateWifiConfiguring){
        SetDeviceState(kDeviceStateIdle);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGI(TAG, "Activation task already running");
            return;
        }        
        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);



    }



}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();
    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        ESP_LOGI(TAG, "Assets partition not valid");
        return;
    }
    Setting settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");
    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

         bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }
    assets.Apply();

}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds
    while(true){
        esp_err_t err = ota_->CheckVersion();
        if(err != ESP_OK){
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
        }

        ota_->MarkCurrentVersionValid();        
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }

    }

}



void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;
    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
        ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));
    SetDeviceState(kDeviceStateUpgrading);

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = ota_->Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
           
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
       
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
return true;

}    

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }    
    protocol_->OnConnected([this]() {

    });

    protocol_->OnErrorCallback([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudioCallback([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        // 通道打开后关闭唤醒词、开启语音前处理与上行编码
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.EnableVoiceProcessing(true);
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(true);
        Schedule([this]() {
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
            auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming json message has no valid type");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            }else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            audio_service_.EnableVoiceProcessing(false);
                            audio_service_.EnableWakeWordDetection(true);
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            audio_service_.EnableVoiceProcessing(true);
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            }else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        }else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        }else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                MCPServer::GetInstance().ParseMessage(payload);
            }
        }else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}


void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {

}



