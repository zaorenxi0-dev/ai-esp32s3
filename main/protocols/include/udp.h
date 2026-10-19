#pragma once

#include <string>
#include <functional>

/**
 * Udp —— UDP 传输抽象接口
 *
 * 与 Tcp 接口保持一致的风格：纯虚方法由具体平台实现（如 EspUdp）。
 * 实时音频数据经 UDP 承载，控制消息经 MQTT 承载（见 mqtt_protocol）。
 */
class Udp {
public:
    virtual ~Udp() = default;

    virtual bool Connect(const std::string& host, int port) = 0;
    virtual void Disconnect() = 0;
    /// 返回发送字节数，<=0 表示失败
    virtual bool Send(const std::string& data) = 0;

    virtual void OnMessage(std::function<void(const std::string& data)> callback) {
        message_callback_ = std::move(callback);
    }

    // 连接状态查询
    bool connected() const { return connected_; }

    // 获取最后一次错误码
    virtual int GetLastError() = 0;

protected:
    std::function<void(const std::string& data)> message_callback_;
    bool connected_ = false;
};
