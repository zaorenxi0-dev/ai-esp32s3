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
    bool Connect(const std::string broker_address, int broker_port, const std::string client_id, const std::string username, const std::string password);
    void Disconnect();
    bool Publish(const std::string& topic, const std::string& payload, int qos = 0);
    bool Subscribe(const std::string& topic, int qos = 0);
    bool Unsubscribe(const std::string& topic);
    bool IsConnected() const;
    int GetLastErrorCode() const;
private:
    std::string message_payload_;
    bool connected_ = false;
    EventGroupHandle_t event_group_handle_;
    esp_mqtt_client_handle_t mqtt_client_handle_ = nullptr;
    int last_error_ = 0;

void MqttEventCallback(esp_event_base_t base, int32_t event_id, void *event_data);
};



