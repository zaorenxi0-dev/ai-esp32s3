#include "wifi_manager.h"

#include <esp_netif.h>

#define TAG "Wifi_Manager"

Wifi_Manager::Wifi_Manager() {}

Wifi_Manager::~Wifi_Manager() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (station_active_ && wifi_station_) {
        wifi_station_->StopStation();
    }
    if (configuration_ap_active_ && wifi_configuration_ap_) {
        wifi_configuration_ap_->StopAp();
    }
    if (initialized_) {
        esp_wifi_deinit();
    }
}

bool Wifi_Manager::Initialize(const WifiManagerConfig& config) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }
    config_ = config;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return false;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return false;
    }

    // 关键：创建 STA 的默认 netif（网络接口）。
    // 原始工程这里缺失这一步 —— 没有 netif 就没有 lwIP 的 DHCP 客户端，
    // 表现为 WiFi 二层关联成功（能打印 L2 connected），但永远等不到 IP：
    //   I (5292) Wifi_Station: L2 connected to WiFi "love", waiting for IP...
    //   ... 60 秒后超时退回配网模式
    // 该函数会挂上默认事件处理（WIFI_EVENT / IP_EVENT），DHCP 拿到地址后
    // 自动向默认事件循环投递 IP_EVENT_STA_GOT_IP，Wifi_Station 才能上报 Connected。
    // 配网 AP 那边是另一条路径（WIFI_AP_DEF），所以单独 AP 模式一直是好的。
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == nullptr) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return false;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_init_cfg.nvs_enable = false;
    if (esp_wifi_init(&wifi_init_cfg) != ESP_OK) {
        return false;
    }

    wifi_station_ = std::make_unique<Wifi_Station>();
    // 把 STA 的连接/断开状态转发成 WifiEvent 通知给上层（WifiBoard）。
    // 原始工程里这三个回调从未注册，因此 WifiEvent::Connected 永远不会触发，
    // 设备即使连上路由器也不会往下走 OTA / MQTT。
    wifi_station_->OnConnect([this](const std::string& ssid) {
        NotifyEvent(WifiEvent::Connecting);
    });
    wifi_station_->OnConnected([this](const std::string& ssid) {
        NotifyEvent(WifiEvent::Connected);
    });
    wifi_station_->OnDisconnect([this]() {
        NotifyEvent(WifiEvent::Disconnected);
    });
    wifi_configuration_ap_ = std::make_unique<Wifi_Configuration_AP>();
    initialized_ = true;
    return true;
}

void Wifi_Manager::SetCallback(std::function<void(WifiEvent)> callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void Wifi_Manager::NotifyEvent(WifiEvent event) {
    auto cb = callback_;
    if (cb) {
        cb(event);
    }
}

std::string Wifi_Manager::GetSsid() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!station_active_ || !wifi_station_) return "";
    return wifi_station_->GetSsid();
}

std::string Wifi_Manager::GetIpAddress() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!station_active_ || !wifi_station_) return "";
    return wifi_station_->GetIpAddress();
}

int Wifi_Manager::GetRssi() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!station_active_ || !wifi_station_) return 0;
    return wifi_station_->GetRssi();
}

int Wifi_Manager::GetChannel() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!station_active_ || !wifi_station_) return 0;
    return wifi_station_->GetChannel();
}

std::string Wifi_Manager::GetMacAddress() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!mac_address_.empty()) {
        return mac_address_;
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        mac_address_ = buf;
    }
    return mac_address_;
}

void Wifi_Manager::StartStation() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_ || !wifi_station_ || station_active_) {
        return;
    }
    if (configuration_ap_active_ && wifi_configuration_ap_) {
        wifi_configuration_ap_->StopAp();
        configuration_ap_active_ = false;
    }
    wifi_station_->StartStation();
    station_active_ = true;
    NotifyEvent(WifiEvent::Connecting);
}

void Wifi_Manager::StopStation() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!station_active_ || !wifi_station_) {
        return;
    }
    wifi_station_->StopStation();
    station_active_ = false;
    NotifyEvent(WifiEvent::Disconnected);
}

void Wifi_Manager::StartConfigurationAp() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_ || !wifi_configuration_ap_ || configuration_ap_active_) {
        return;
    }
    if (station_active_ && wifi_station_) {
        wifi_station_->StopStation();
        station_active_ = false;
    }
    wifi_configuration_ap_->SetSsidPrefix(config_.ssid_prefix);
    wifi_configuration_ap_->SetLanguage(config_.language);
    wifi_configuration_ap_->OnExitRequest([this]() {
        this->StopConfigurationAp();
    });
    wifi_configuration_ap_->StartAp();
    configuration_ap_active_ = true;
    NotifyEvent(WifiEvent::ConfigModeEnter);
}

void Wifi_Manager::StopConfigurationAp() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!configuration_ap_active_ || !wifi_configuration_ap_) {
        return;
    }
    wifi_configuration_ap_->StopAp();
    configuration_ap_active_ = false;
    NotifyEvent(WifiEvent::ConfigModeExit);
}

bool Wifi_Manager::IsConfigMode() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return configuration_ap_active_;
}

std::string Wifi_Manager::GetApSsid() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!wifi_configuration_ap_) return "";
    return wifi_configuration_ap_->GetSsid();
}

std::string Wifi_Manager::GetApWebUrl() const {
    return "http://192.168.4.1";
}

void Wifi_Manager::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (wifi_station_) {
        wifi_station_->SetPowerSaveLevel(level);
    }
}
