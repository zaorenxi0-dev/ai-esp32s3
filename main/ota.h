#pragma once

#include <esp_err.h>
#include <esp_log.h>

#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <cJSON.h>

#include "board.h"
#include "http_client.h"
#include "setting.h"
#include "http.h"

#include <esp_efuse.h>
#include <esp_efuse_table.h>

class Ota {
public:
    Ota();
    ~Ota();

    esp_err_t CheckVersion();
    std::string GetCheckVersionUrl();
    bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    void MarkCurrentVersionValid();
    bool HasActivationCode() { return has_activation_code_; }
    bool HasActivationChallenge() { return has_activation_challenge_; }
    esp_err_t Activate();
    std::string GetActivationPayload();
    bool HasServerTime() { return has_server_time_; }
private:
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;



    std::string activation_code_;
    std::string activation_message_;
    std::string current_version_;
    std::string serial_number_;
    std::string activation_challenge_;
    std::string firmware_version_;
    std::string firmware_url_;

    int activation_timeout_ms_ = 30000;
    bool has_new_version_ = false;

    std::unique_ptr<Http> SetupHttp();
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);

};
