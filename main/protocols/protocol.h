#pragma once
#include <esp_err.h>
#include <esp_log.h>


#include <cJSON.h>
#include <string>
#include <functional>
#include <vector>
#include <chrono>
#include <memory>

//sample_rate：采样率（int，默认 0）
//frame_duration：帧时长（int，默认 0）
//timestamp：时间戳（uint32_t，默认 0）
//payload：音频数据载荷（std::vector<uint8_t> 字节容器）
struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};



class Protocol
{
public:
    Protocol();
    ~Protocol();
    
    void OnErrorCallback(std::function<void(const std::string&)> callback);
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    void OnIncomingAudioCallback(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    void OnAudioChannelOpened(std::function<void()> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

    virtual bool Start() = 0;
    virtual bool OpenAudioChannel() = 0;
    virtual void CloseAudioChannel() = 0;
    virtual bool IsAudioChannelOpened() const = 0;
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;

    // 发送 MCP (JSON-RPC) 消息给服务端/客户端（底层复用控制通道 SendText）
    virtual bool SendMcpMessage(const std::string& message);

    // 通知服务端「用户打断，别说了」。
    // 服务端 abortHandle 收到 type=abort 后会 client_abort=True + clear_queues()，
    // 并回一条 {"type":"tts","state":"stop"}——设备侧的 tts stop 分支已经能处理它，
    // 所以设备只管发，不用自己造状态。
    virtual bool SendAbort();

    inline int server_sample_rate() const {
        return server_sample_rate_;
    }
    inline int server_frame_duration() const {
        return server_frame_duration_;
    }

protected:

    std::function<void(const std::string&)> on_error_callback_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void()> on_audio_channel_closed_;


    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;


    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    bool error_occurred_ = false;
    std::string session_id_;

    virtual bool SendText(const std::string& text) = 0;
    virtual void SetError(const std::string& message);
    virtual bool IsTimeout() const;

};

