#include "wifi_board.h"
#include <esp_log.h>
#include <esp_err.h>

#define TAG "WifiBoard"
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard()
{
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard()
{
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

void WifiBoard::StartNetwork()
{
    auto& wifi_manager = Wifi_Manager::GetInstance();

    ESP_LOGI(TAG, "StartNetwork");
    WifiManagerConfig wifi_manager_config;
    wifi_manager_config.ssid_prefix = "Xiaozhi";
    wifi_manager_config.language = Lang::CODE;
    wifi_manager.Initialize(wifi_manager_config);

    wifi_manager.SetCallback([this, &wifi_manager](WifiEvent event) {
        ESP_LOGI(TAG, "WifiEvent: %d", event);
        std::string ssid = wifi_manager.GetSsid();
        switch (event) {
        case WifiEvent::Scanning:
            OnNetworkEvent(NetworkEvent::Scanning);
            break;
        case WifiEvent::Connecting:
            OnNetworkEvent(NetworkEvent::Connecting, ssid);
            break;
        case WifiEvent::Connected:
            OnNetworkEvent(NetworkEvent::Connected, ssid);
            break;
        case WifiEvent::Disconnected:
            OnNetworkEvent(NetworkEvent::Disconnected);
            break;
        case WifiEvent::ConfigModeEnter:
            OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
            break;
        case WifiEvent::ConfigModeExit:
            OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
            break;
        default:
            break;
        }
    });

    TryWifiConnect();

}

void WifiBoard::OnNetworkEvent(NetworkEvent event,const std::string &data)
{
    switch (event) {
        case NetworkEvent::Connected:
            esp_timer_stop(connect_timer_);
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Successfully connected to network: %s", data.c_str());
            break;

        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "Scanning for networks");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "Connecting to network: %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGI(TAG, "Disconnected from network: %s", data.c_str());
            break;
        case NetworkEvent::WifiConfigModeEnter:
            in_config_mode_ = true;
            ESP_LOGI(TAG, "Entered config AP mode");
            break;
        case NetworkEvent::WifiConfigModeExit:
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Exited config AP mode");
            TryWifiConnect();
            break;
        default:
            break;
    }

    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::OnWifiConnectTimeout(void* arg)
{
    WifiBoard* wifi_board = static_cast<WifiBoard*>(arg);
    ESP_LOGI(TAG, "Wifi connection timeout");

    // 本函数运行在 esp_timer 回调上下文，timer task 栈只有约 3.5KB。
    // StopStation / StartWifiConfigMode 内部要构造 httpd、DNS server、std::function
    // 等重量级对象，直接在这里执行有爆栈风险，因此交给独立任务处理。
    BaseType_t ok = xTaskCreate([](void* p) {
        WifiBoard* board = static_cast<WifiBoard*>(p);
        Wifi_Manager::GetInstance().StopStation();
        board->StartWifiConfigMode();
        vTaskDelete(nullptr);
    }, "wifi_timeout", 6144, wifi_board, 4, nullptr);

    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create timeout task, falling back to direct call");
        Wifi_Manager::GetInstance().StopStation();
        wifi_board->StartWifiConfigMode();
    }
}

void WifiBoard::TryWifiConnect()
{
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }else{
        ESP_LOGI(TAG, "Try to connect to SSID: %s", ssid_list[0].ssid.c_str());
        esp_timer_start_once(connect_timer_,  CONNECT_TIMEOUT_SEC * 1000000ULL);
        auto& wifi_manager = Wifi_Manager::GetInstance();
        wifi_manager.StartStation();
    }
}

void WifiBoard::StartWifiConfigMode()
{
    in_config_mode_ = true;
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
    
    auto& wifi_manager = Wifi_Manager::GetInstance();
    wifi_manager.StartConfigurationAp();

    Application::GetInstance().Schedule([this]() {
        auto& wifi_manager = Wifi_Manager::GetInstance();
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
    
}

void WifiBoard::EnterWifiConfigMode(){
    ESP_LOGI(TAG, "EnterWifiConfigMode");

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking||state == kDeviceStateIdle||state == kDeviceStateListening) {
        app.ResetProtocol();

        xTaskCreate([](void* arg) {
            WifiBoard* wifi_board = static_cast<WifiBoard*>(arg);

            vTaskDelay(pdMS_TO_TICKS(1000));
            // Stop any ongoing connection attempt
            esp_timer_stop(wifi_board->connect_timer_);
            Wifi_Manager::GetInstance().StopStation();

            wifi_board->StartWifiConfigMode();
            vTaskDelay(NULL);
        }, "wifi_config_task", 4096, this, 2, nullptr);
    }

    if (state != kDeviceStateStarting) {
        ESP_LOGI(TAG, "EnterWifiConfigMode called but device state is not starting or speaking, device state: %d", state);
        return;
    }
    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    Wifi_Manager::GetInstance().StopStation();

    StartWifiConfigMode();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return Wifi_Manager::GetInstance().IsConfigMode();
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = Wifi_Manager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + wifi.GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    Wifi_Manager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

     // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Network
    auto& wifi = Wifi_Manager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;

}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork esp_network;
    return &esp_network;
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}
