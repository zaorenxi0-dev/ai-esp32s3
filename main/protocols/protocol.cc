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

bool Protocol::SendAbort() {
    // 与官方 xiaozhi-esp32 的 Protocol::SendAbortSpeaking() 完全一致的报文格式：
    //   {"session_id":"...","type":"abort","reason":"wake_word_detected"}
    // session_id 让服务端确认是哪一轮会话（重连/多会话时不会误杀别的会话），
    // reason 服务端目前不解析，但日志里能看出打断来源，排查有用。
    //
    // 服务端 abortHandle.py 只读 type 字段，所以原来只发 {"type":"abort"} 也能工作；
    // 补齐这两个字段是为了对齐标准协议，不是修 bug。
    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"abort\",";
    message += "\"reason\":\"wake_word_detected\"";
    message += "}";
    return SendText(message);
}

