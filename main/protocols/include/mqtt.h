#pragma once
#include <string>
#include <functional>




class Mqtt {
public:
    virtual ~Mqtt() = default;
    virtual void SetKeepaliveInterval(int keepalive_interval) {keepalive_interval_ = keepalive_interval;}
    virtual bool Connect(const std::string& broker_address, int broker_port, const std::string& client_id, const std::string& username, const std::string& password) {
        (void)broker_address; (void)broker_port; (void)client_id; (void)username; (void)password;
        return false;
    }
    virtual void Disconnect() {}
    virtual bool Publish(const std::string& topic, const std::string& payload, int qos = 0) {
        (void)topic; (void)payload; (void)qos;
        return false;
    }
    virtual bool Subscribe(const std::string& topic, int qos = 0) {
        (void)topic; (void)qos;
        return false;
    }
    virtual bool Unsubscribe(const std::string& topic) {
        (void)topic;
        return false;
    }
    virtual bool IsConnected() const { return false; }

    virtual void OnConnected(std::function<void()> callback) { on_connected_callback_ = std::move(callback); }
    virtual void OnDisconnected(std::function<void()> callback) { on_disconnected_callback_ = std::move(callback); }
    virtual void OnMessage(std::function<void(const std::string& topic, const std::string& payload)> callback) { on_data_received_callback_ = std::move(callback); }
    virtual void OnError(std::function<void(const std::string& error)> callback) { on_error_callback_ = std::move(callback); }
    
protected:
    int keepalive_interval_=120;

    std::function<void()> on_connected_callback_;
    std::function<void()> on_disconnected_callback_;
    std::function<void(const std::string& topic, const std::string& payload)> on_data_received_callback_;
    std::function<void(const std::string& error)> on_error_callback_;


};
