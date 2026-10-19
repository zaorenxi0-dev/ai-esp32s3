#pragma once

#include <string>
#include <vector>
#include "tcp.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_err.h>
#include <esp_netif.h>

#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#define ESP_TCP_EVENT_RECEIVE_TASK_EXIT 1
class EspTcp : public Tcp {
public:
    EspTcp();
    virtual ~EspTcp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;
    int GetLastError() override;
private:
    int tcp_fd_ = -1;
    EventGroupHandle_t event_group_ = nullptr;
    int last_error_ = 0;
    TaskHandle_t receive_task_handle_ = nullptr;
    void ReceiveTask();    
    void DoDisconnect(bool wait_for_task);
    // 连接状态管理
    bool connected_ = false;         // 是否可以正常读写数据
};

