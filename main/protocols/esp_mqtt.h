#pragma once

#include <string>
#include <memory>
#include <functional>
#include <vector>

#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "esp_event.h"
#include "mqtt.h"

#define MQTT_CONNECT_TIMEOUT_MS 10000

#define MQTT_INITIALIZED_EVENT BIT0
#define MQTT_CONNECTED_EVENT BIT1
#define MQTT_DISCONNECTED_EVENT BIT2
#define MQTT_ERROR_EVENT BIT3

class EspMqtt : public Mqtt {
public:
    EspMqtt();
    ~EspMqtt();
    // 参数必须与基类 Mqtt::Connect 完全一致（全部是 const std::string&）。
    // 原实现写成了按值传递 const std::string，签名不同 → 不构成 override，
    // 调用方（MqttProtocol，持有 unique_ptr<Mqtt>）走的是基类那个
    // "直接 return false" 的默认实现，本函数成为死代码被 --gc-sections 丢弃。
    // 症状：connect 立即失败、耗时 0ms、函数内任何日志都不会打印。
    bool Connect(const std::string& broker_address, int broker_port,
                 const std::string& client_id, const std::string& username,
                 const std::string& password) override;
    void Disconnect() override;
    bool Publish(const std::string& topic, const std::string& payload, int qos = 0) override;
    bool Subscribe(const std::string& topic, int qos = 0) override;
    bool Unsubscribe(const std::string& topic) override;
    bool IsConnected() const override;
    int GetLastErrorCode() const;
private:
    std::string message_payload_;
    bool connected_ = false;
    EventGroupHandle_t event_group_handle_;
    esp_mqtt_client_handle_t mqtt_client_handle_ = nullptr;
    int last_error_ = 0;

void MqttEventCallback(esp_event_base_t base, int32_t event_id, void *event_data);
};



