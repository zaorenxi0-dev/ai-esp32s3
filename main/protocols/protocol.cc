#include "protocol.h"
#include <esp_log.h>

#define TAG "Protocol"

Protocol::Protocol()
{
    
}
Protocol::~Protocol()
{

}


void Protocol::OnErrorCallback(std::function<void(const std::string&)> callback)
{
    on_error_callback_ = callback;
}
void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudioCallback(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback)
{
    on_incoming_audio_ = callback;
}
void Protocol::OnAudioChannelOpened(std::function<void()> callback)
{
    on_audio_channel_opened_ = callback;
}
void Protocol::OnAudioChannelClosed(std::function<void()> callback)
{
    on_audio_channel_closed_ = callback;
}
void Protocol::OnConnected(std::function<void()> callback)
{
    on_connected_ = callback;
}
void Protocol::OnDisconnected(std::function<void()> callback)
{
    on_disconnected_ = callback;
}
void Protocol::SetError(const std::string& error)
{
    error_occurred_ = true;
    if(on_error_callback_!=nullptr)
    {
        on_error_callback_(error);
    }
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}

bool Protocol::SendMcpMessage(const std::string& message) {
    // MCP 的 JSON-RPC 消息通过控制通道（文本）发送
    return SendText(message);
}

