#pragma once

#include "esp_log.h"
#include "esp_err.h"

#include "wifi_manager.h"
#include "lang_config.h"
#include "ssid_manager.h"
#include "application.h"
#include "esp_network.h"
#include "boards/common/board.h"


#include  <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


class WifiBoard : public Board{
public:
    WifiBoard();
    ~WifiBoard();

    void StartNetwork();
    bool IsInWifiConfigMode() const;
    virtual std::string GetDeviceStatusJson() override;
    void EnterWifiConfigMode();
    std::string GetBoardJson() override;
    void SetPowerSaveLevel(PowerSaveLevel level);
    NetworkInterface* GetNetwork();
    void SetNetworkEventCallback(NetworkEventCallback callback) override;

private:
    esp_timer_handle_t connect_timer_ = nullptr;



    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");
   static void OnWifiConnectTimeout(void *arg);
    void StartWifiConfigMode();
    void TryWifiConnect();

    bool in_config_mode_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;
};
