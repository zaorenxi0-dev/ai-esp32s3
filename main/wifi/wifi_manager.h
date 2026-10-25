#pragma once

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include "wifi_station.h"
#include "wifi_configuration_ap.h"


#include <memory>
#include <string>
#include <mutex>
#include <esp_wifi.h>
#include <esp_mac.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>

// WiFi events
enum class WifiEvent {
    Scanning,          // Started scanning for networks
    Connecting,        // Connecting to network (call GetSsid() for target)
    Connected,         // Successfully connected
    Disconnected,      // Disconnected from network
    ConfigModeEnter,   // Entered config AP mode
    ConfigModeExit,    // Exited config AP mode
};


struct WifiManagerConfig
{
    std::string ssid_prefix = "ESP32";
    std::string language = "zh-CN";

    int station_scan_min_interval_seconds = 10;
    int station_scan_max_interval_seconds = 300;

};


class Wifi_Manager
{
public:
    static Wifi_Manager& GetInstance() {
        static Wifi_Manager instance;
        return instance;
    }

    bool Initialize(const WifiManagerConfig& config);
//station
    std::string GetSsid();
    void StartStation();
    void StopStation();
    std::string GetIpAddress() const;
    std::string GetMacAddress() const;
    int GetRssi() const;
    int GetChannel() const;
//ap
    void StartConfigurationAp();
    void StopConfigurationAp();
    bool IsConfigMode() const;
    std::string GetApSsid() const;
    std::string GetApWebUrl() const;

//event

    void SetCallback(std::function<void(WifiEvent)> callback);
    
    void SetPowerSaveLevel(WifiPowerSaveLevel level);

private:
    Wifi_Manager();
    ~Wifi_Manager();

    void NotifyEvent(WifiEvent event);

    WifiManagerConfig config_;
    // 注意：必须是递归锁。
    // NotifyEvent() 会在持锁状态下同步调用上层回调，而回调里（WifiBoard 的 lambda）
    // 会调用 GetSsid()/GetIpAddress() 等再次加锁的接口。
    // 用非递归的 std::mutex 会自死锁 —— 表现为：连不上 WiFi 时打印完
    // "WifiEvent: 1" 就卡死，60 秒后再打印 "Wifi connection timeout"，之后整机无响应。
    mutable std::recursive_mutex mutex_;
    std::function<void(WifiEvent)> callback_;
    std::unique_ptr<Wifi_Station> wifi_station_;
    std::unique_ptr<Wifi_Configuration_AP> wifi_configuration_ap_;


    bool initialized_ = false;
    bool station_active_ = false;
    bool configuration_ap_active_ = false;
    mutable std::string mac_address_;

};
