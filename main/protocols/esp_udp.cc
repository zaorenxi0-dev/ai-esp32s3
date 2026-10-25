#include "esp_udp.h"
#include <esp_log.h>
#include <esp_err.h>



#define TAG "EspUdp"

EspUdp::EspUdp(): udp_fd_(-1)
{
        event_group_ = xEventGroupCreate();
}

EspUdp::~EspUdp()
{
        Disconnect();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspUdp::Connect(const std::string& host, int port)
{
    if(connected_)
    {
        Disconnect();
    }
    struct sockaddr_in server_addr = {0};
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    struct hostent* server = gethostbyname(host.c_str());
    if(server == nullptr)
    {
        ESP_LOGE(TAG, "Failed to get host by name: %s", host.c_str());
        last_error_ = errno;
        return false;
    }
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if(udp_fd_ < 0)
    {
        ESP_LOGE(TAG, "socket failed");
        last_error_ = errno;
        return false;
    }
    int ret = connect(udp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if(ret < 0)
    {
        ESP_LOGE(TAG, "connect failed");
        last_error_ = errno;
        close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }
    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT);
    // 栈必须给够：这个任务不只做 recv，回调链是
    //   ReceiveTask -> message_callback_(MqttProtocol 里的解密 lambda)
    //   -> mbedtls_aes_crypt_ctr（栈上 stream_block[16] + mbedtls 内部帧）
    //   -> on_incoming_audio_(Application lambda) -> AudioService::PushPacketToDecodeQueue
    // 原来只给了 2048，上行方向走不到这条链所以一直没暴露；
    // 一旦服务端下发第一包 TTS 音频就 "A stack overflow in task udp_receive"
    // 直接 panic 重启。对比 tcp_receive 用的 4096 也只是勉强。
    xTaskCreate([](void* arg)
    {
        EspUdp* esp_udp = (EspUdp*)arg;
        esp_udp->ReceiveTask();
        xEventGroupSetBits(esp_udp->event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT);
         vTaskDelete(nullptr);
    }, "udp_receive", 6144, this, 5, &receive_task_handle_);
    return true;
}

void EspUdp::Disconnect()
{
    connected_ = false;
    if(udp_fd_ >= 0)
    {
        close(udp_fd_);
        udp_fd_ = -1;
        auto bits = xEventGroupWaitBits(event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_UDP_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGI(TAG, "Failed to wait for receive task exit");
        }
    }
}

void EspUdp::ReceiveTask()
{
    std::string data;
    while(connected_)
    {
        data.resize(1500);
        int ret = recv(udp_fd_, (char*)data.data(), data.size(), 0);
        if(ret <=0)
        {
            connected_ = false;
            break;
        }

        if(message_callback_){
            data.resize(ret);
            message_callback_(data);
        }
    }


}


bool EspUdp::Send(const std::string& data)
{
    if(!connected_)
    {
        ESP_LOGI(TAG, "Not connected");
        return -1;
    }
    int ret = send(udp_fd_, data.c_str(), data.size(), 0);
    if(ret < 0)
    {
        ESP_LOGI(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
        return false;
    }
    return ret;
}
int EspUdp::GetLastError() {
    return last_error_;
}
