#pragma once

#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif.h>

#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * 极简 DNS 服务器：配合 WiFi 配网 AP 做 "强制门户"(captive portal)。
 * 对任意域名的 A 记录查询统一返回 AP 自身的网关地址（默认 192.168.4.1），
 * 从而让手机在连接热点后自动弹出配网页。
 */
class DnsServer {
public:
    DnsServer();
    ~DnsServer();

    /// 启动 DNS 服务，gateway 为要返回的地址（通常是 192.168.4.1）
    void Start(esp_ip4_addr_t gateway);
    /// 停止 DNS 服务
    void Stop();
    bool IsRunning() const { return running_; }

private:
    static void DnsTask(void* arg);
    void ProcessRequest();

    TaskHandle_t task_handle_ = nullptr;
    int sock_ = -1;
    esp_ip4_addr_t gateway_{};
    volatile bool running_ = false;
};
