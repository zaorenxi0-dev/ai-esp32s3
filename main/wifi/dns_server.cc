#include "dns_server.h"

#include <cstring>

#include <lwip/sockets.h>
#include <lwip/inet.h>

#define TAG "DnsServer"
#define DNS_PORT 53
#define MAX_DNS_PACKET 512

DnsServer::DnsServer() {
}

DnsServer::~DnsServer() {
    Stop();
}

void DnsServer::Start(esp_ip4_addr_t gateway) {
    if (running_) {
        return;
    }
    gateway_ = gateway;

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket: errno %d", errno);
        return;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DNS_PORT);
    if (bind(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket: errno %d", errno);
        close(sock_);
        sock_ = -1;
        return;
    }

    running_ = true;
    xTaskCreate(DnsTask, "dns_server", 4096, this, 5, &task_handle_);
    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);
}

void DnsServer::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    if (sock_ >= 0) {
        // 关闭 socket 让阻塞的 recvfrom 立即返回
        shutdown(sock_, SHUT_RDWR);
        close(sock_);
        sock_ = -1;
    }
    task_handle_ = nullptr;
    ESP_LOGI(TAG, "DNS server stopped");
}

void DnsServer::DnsTask(void* arg) {
    DnsServer* server = static_cast<DnsServer*>(arg);
    while (server->running_) {
        server->ProcessRequest();
    }
    vTaskDelete(nullptr);
}

void DnsServer::ProcessRequest() {
    uint8_t buffer[MAX_DNS_PACKET];
    struct sockaddr_in client = {};
    socklen_t client_len = sizeof(client);

    int len = recvfrom(sock_, buffer, sizeof(buffer), 0, (struct sockaddr*)&client, &client_len);
    if (len < 12) {  // 最小 DNS 头长度
        return;
    }

    // 只处理标准查询（QR = 0）
    if (buffer[2] & 0x80) {
        return;
    }

    // 构造响应：复制请求，设置 QR=1、RA=1、RCODE=0，ANCOUNT=1
    uint8_t response[MAX_DNS_PACKET];
    memcpy(response, buffer, len);

    response[2] |= 0x80;           // QR = 1
    response[3] = (response[3] & 0xF0) | 0x00;
    response[3] |= 0x80;           // RA = 1
    response[6] = 0x00;            // ANCOUNT high
    response[7] = 0x01;            // ANCOUNT low = 1

    // 在请求末尾追加一条 A 记录回答（name 用压缩指针 0xC00C 指向 question 的名字）
    if (len + 16 > MAX_DNS_PACKET) {
        return;
    }
    int pos = len;
    response[pos++] = 0xC0;
    response[pos++] = 0x0C;        // 指向偏移 12 的 QNAME
    response[pos++] = 0x00;
    response[pos++] = 0x01;        // TYPE = A
    response[pos++] = 0x00;
    response[pos++] = 0x01;        // CLASS = IN
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x3C;        // TTL = 60s
    response[pos++] = 0x00;
    response[pos++] = 0x04;        // RDLENGTH = 4
    response[pos++] = esp_ip4_addr1(&gateway_);
    response[pos++] = esp_ip4_addr2(&gateway_);
    response[pos++] = esp_ip4_addr3(&gateway_);
    response[pos++] = esp_ip4_addr4(&gateway_);

    sendto(sock_, response, pos, 0, (struct sockaddr*)&client, client_len);
}
