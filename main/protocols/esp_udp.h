#pragma once

#include <memory>
#include <string>

#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <arpa/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "udp.h"

#define ESP_UDP_EVENT_RECEIVE_TASK_EXIT 1
class EspUdp : public Udp{
public:
    EspUdp();
    ~EspUdp();

    // 全部加 override：签名一旦和基类 Udp 不一致，编译器会直接报错，
    // 避免出现"以为覆盖了、实际调用基类空实现"的隐性故障（EspMqtt 已踩过）。
    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    void ReceiveTask();
    bool Send(const std::string& data) override;
    int GetLastError() override;
private:
    EventGroupHandle_t event_group_;
    TaskHandle_t receive_task_handle_ = nullptr;
    int udp_fd_;
    int last_error_ = 0;
};
