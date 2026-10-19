#include "wifi_station.h"

#define TAG "Wifi_Station"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1

Wifi_Station::Wifi_Station() {
    event_group_ = xEventGroupCreate();
}

Wifi_Station::~Wifi_Station() {
    StopStation();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void Wifi_Station::OnScanBegin(std::function<void()> callback) { on_scan_begin_ = std::move(callback); }
void Wifi_Station::OnConnect(std::function<void(const std::string& ssid)> callback) { on_connect_ = std::move(callback); }
void Wifi_Station::OnConnected(std::function<void(const std::string& ssid)> callback) { on_connected_ = std::move(callback); }
void Wifi_Station::OnDisconnect(std::function<void()> callback) { on_disconnect_ = std::move(callback); }

void Wifi_Station::StartStation() {
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    if (on_scan_begin_) {
        on_scan_begin_();
    }
}

void Wifi_Station::StopStation() {
    esp_wifi_stop();
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
    if (on_disconnect_) {
        on_disconnect_();
    }
}

bool Wifi_Station::IsConnected() {
    return (xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED) != 0;
}

bool Wifi_Station::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED,
                                    pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

int8_t Wifi_Station::GetRssi() {
    if (!IsConnected()) {
        return 0;
    }
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.rssi;
}

uint8_t Wifi_Station::GetChannel() {
    if (!IsConnected()) {
        return 0;
    }
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.primary;
}

void Wifi_Station::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (level == WifiPowerSaveLevel::LOW_POWER) {
        ps = WIFI_PS_MAX_MODEM;
    } else if (level == WifiPowerSaveLevel::BALANCED) {
        ps = WIFI_PS_MIN_MODEM;
    }
    esp_wifi_set_ps(ps);
}

void Wifi_Station::HandleScanResult() {}
void Wifi_Station::UpdateScanInterval() {}
void Wifi_Station::StartConnect() {}
