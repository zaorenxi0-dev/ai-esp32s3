#pragma once

#include <esp_log.h>
#include <esp_err.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <esp_timer.h>
#include <atomic>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mutex>
#include <cJSON.h>

#include "esp_udp.h"
#include "application.h"
#include "protocol.h"
#include "protocols/include/mqtt.h"
#include "setting.h"
#include "assets/lang_config.h"
#define MQTT_RECONNECT_INTERVAL_MS 60000
#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class MqttProtocol : public Protocol
{
public:
    MqttProtocol();
    ~MqttProtocol();

    bool OpenAudioChannel();
    bool Start();
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet);
    void CloseAudioChannel();
    bool IsAudioChannelOpened() const;

private:
 // Alive flag for safe scheduled callbacks - set to false in destructor
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    std::string publish_topic_;
    esp_timer_handle_t reconnect_timer_;
    std::string udp_server_;
    std::string aes_nonce_;
    mbedtls_aes_context aes_ctx_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    int udp_port_;
    EventGroupHandle_t event_group_;
    std::mutex channel_mutex_;

    std::string GetHelloMessage();
    bool SendText(const std::string& text);
    bool StartMqttClient(bool report_error);
    void ParseServeHello(cJSON* root);
    std::string DecodeHexString(const std::string& hex_string);
};
