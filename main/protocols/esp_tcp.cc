#include "esp_tcp.h"

#include <esp_log.h>
#include <esp_err.h>

#define TAG "EspTcp"



EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    Disconnect();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }

}

bool EspTcp::Connect(const std::string& host, int port) {
    if (connected_) {
       Disconnect();
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        ESP_LOGI(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGI(TAG, "Failed to create socket");
        return false;
    }
    int ret = connect(tcp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        last_error_ = errno;
        ESP_LOGI(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }
    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        tcp->ReceiveTask();
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task_handle_);
    return true;

}

void EspTcp::Disconnect() {
    if (!connected_) {
        return;
    }
    DoDisconnect(true);
}

int EspTcp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGI(TAG, "TCP send failed: not connected");
        return -1;
    }
    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        int ret = send(tcp_fd_, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGI(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;

}



void EspTcp::ReceiveTask() 
{
    std::string data;
    while(connected_)
    {
        data.resize(1500);
        int ret = recv(tcp_fd_, (char*)data.data(), data.size(), 0);
        if(ret <=0)
        {
            if (ret < 0) {
                ESP_LOGI(TAG, "TCP receive failed: %d", ret);
            }
            // 被动断开，不需要等待接收任务退出（当前就是接收任务）
            DoDisconnect(false);
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

void EspTcp::DoDisconnect(bool wait_for_task) 
{
    connected_ = false;
    if(tcp_fd_ >= 0)
    {
        close(tcp_fd_);
        tcp_fd_ = -1;
        if (wait_for_task) {
            auto bits = xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
            if (!(bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT)) {
                ESP_LOGI(TAG, "Failed to wait for receive task exit");
            }
        }
    }
    // 断开连接时触发断开回调
    if (disconnect_callback_) {
        disconnect_callback_();
    }

}
int EspTcp::GetLastError() {
    return last_error_;
}
