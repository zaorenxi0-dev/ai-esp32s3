#pragma once

#include <esp_err.h>
#include <esp_log.h>


#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <esp_event.h>
#include <esp_timer.h>

#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_wifi.h"
#include "ssid_manager.h"

// WiFi power save level enumeration
enum class WifiPowerSaveLevel {
    LOW_POWER,    // Maximum power saving (WIFI_PS_MAX_MODEM)
    BALANCED,     // Minimum power saving (WIFI_PS_MIN_MODEM)
    PERFORMANCE,  // No power saving (WIFI_PS_NONE) - full power
};
struct WifiApRecord {
    std::string ssid;
    std::string password;
    int channel;
    wifi_auth_mode_t authmode;
    uint8_t bssid[6];
};
class Wifi_Station
{
private:
    std::string ssid_;
    std::string password_;
    std::string ip_address_;
    EventGroupHandle_t event_group_;
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
    int8_t max_tx_power_=0;
    bool remember_bssid_=false;
    esp_timer_handle_t scan_timer_ = nullptr;
    std::vector<WifiApRecord> connect_queue_;
    int reconnect_count_ = 0;

    std::function<void()> on_scan_begin_;
    std::function<void(const std::string& ssid)> on_connect_;
    std::function<void(const std::string& ssid)> on_connected_;
    std::function<void()> on_disconnect_;
    bool was_connected_ = false;

    int scan_current_interval_microseconds_ = 10 * 1000 * 1000; 
    int scan_max_interval_microseconds_ = 300 * 1000 * 1000; 
    void HandleScanResult();
    void UpdateScanInterval();
    void StartConnect();
public:
    Wifi_Station();
    ~Wifi_Station();

    void OnScanBegin(std::function<void()> callback);
    void OnConnect(std::function<void(const std::string& ssid)> callback);
    void OnConnected(std::function<void(const std::string& ssid)> callback);
    void OnDisconnect(std::function<void()> callback);
    

    bool IsConnected();
    bool WaitForConnected(int timeout_ms = 10000);
    int8_t GetRssi();
    std::string GetSsid() const { return ssid_; }
    std::string GetIpAddress() const { return ip_address_; }
    uint8_t GetChannel();
    void SetPowerSaveLevel(WifiPowerSaveLevel level);

    void StartStation();
    void StopStation();
};
