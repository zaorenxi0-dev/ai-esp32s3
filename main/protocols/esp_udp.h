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

    bool Connect(const std::string& host, int port);
    void Disconnect();
    void ReceiveTask();
    bool Send(const std::string& data);
    int GetLastError();
private:
    EventGroupHandle_t event_group_;
    TaskHandle_t receive_task_handle_ = nullptr;
    int udp_fd_;
    int last_error_ = 0;
};
