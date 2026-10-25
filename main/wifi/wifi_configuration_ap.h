#pragma once

#include <esp_err.h>
#include <esp_log.h>

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <esp_event.h>
#include <cJSON.h>

#include <esp_timer.h>
#include <esp_mac.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <esp_smartconfig.h>
#include <esp_http_server.h>
#include "esp_wifi.h"
#include <lwip/ip_addr.h>
#include <esp_netif.h>
#include "dns_server.h"
class Wifi_Configuration_AP
{
public:
    Wifi_Configuration_AP();
    ~Wifi_Configuration_AP();
    void StartAp();
    void StopAp();
    void Save(const std::string &ssid, const std::string &password);
    void SetSsidPrefix(const std::string& ssid);
    void SetLanguage(const std::string& language);
    void OnExitRequest(const std::function<void()>& exit_callback);
    bool ConnectToWifi(const std::string &ssid, const std::string &password);
    std::string GetSsid();
    std::string GetWebServerUrl();
    void StartSmartConfig();
    std::vector<wifi_ap_record_t> GetAccessPoints();
private:
    std::string ssid_prefix_;
    std::string language_;
    httpd_handle_t server_ = NULL;
    std::function<void()> on_exit_requested_;
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
    esp_netif_t* ap_netif_ = nullptr;
    std::unique_ptr<DnsServer> dns_server_;
    EventGroupHandle_t event_group_;
    bool is_connecting_ = false;
    std::mutex mutex_;
    std::vector<wifi_ap_record_t> ap_records_;
    esp_timer_handle_t scan_timer_ = nullptr;
    esp_event_handler_instance_t sc_event_instance_ = nullptr;
    // 高级配置项
    std::string ota_url_;
    int8_t max_tx_power_;
    bool remember_bssid_;
    bool sleep_mode_;
    void StartAccessPoint();
    void StartWebServer();

    static void SmartConfigEventHandler(void *arg, esp_event_base_t event_base,int32_t event_id, void *event_data);
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data); 
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
};
