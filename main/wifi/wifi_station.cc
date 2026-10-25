#include "wifi_station.h"

#include <cstring>

#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "Wifi_Station"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1

// 说明：本工程拿到的原始版本里，Wifi_Station 的 STA 连接逻辑被整段删空了：
//   - 没有任何 esp_event_handler_register
//   - StartConnect() / HandleScanResult() / UpdateScanInterval() 全是空函数
//   - on_connect_ / on_connected_ / on_disconnect_ 从未被调用
// 结果就是设备重启后只执行 esp_wifi_start()，从不 esp_wifi_connect()，
// 表现为「永远连不上已保存的 WiFi，60 秒后必定退回配网模式」。
// 下面补齐最小可用的连接流程。

Wifi_Station::Wifi_Station() {
    event_group_ = xEventGroupCreate();

    // 注册 WiFi 事件与 IP 事件（在 Wifi_Manager::Initialize 里完成
    // esp_event_loop_create_default() 与 esp_wifi_init() 之后才会构造本对象）
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &Wifi_Station::WifiEventHandler, this, &instance_any_id_);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &Wifi_Station::IpEventHandler, this, &instance_got_ip_);
}

Wifi_Station::~Wifi_Station() {
    if (instance_any_id_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }
    StopStation();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void Wifi_Station::OnScanBegin(std::function<void()> callback) { on_scan_begin_ = std::move(callback); }
void Wifi_Station::OnConnect(std::function<void(const std::string& ssid)> callback) { on_connect_ = std::move(callback); }
void Wifi_Station::OnConnected(std::function<void(const std::string& ssid)> callback) { on_connected_ = std::move(callback); }
void Wifi_Station::OnDisconnect(std::function<void()> callback) { on_disconnect_ = std::move(callback); }

void Wifi_Station::StartStation() {
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED);
    station_started_ = true;
    was_connected_ = false;
    reconnect_count_ = 0;
    ip_address_.clear();

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    // 连接动作必须放到独立任务里执行。
    // StartConnect() 会构造 wifi_config_t、做 vector<SsidItem> 拷贝，并通过
    // NotifyEvent 一路调到显示层。如果直接在事件任务（sys_evt）里跑，会栈溢出
    // ——本工程 CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE 原始值只有 2304 字节，
    // 表现为 "A stack overflow in task sys_evt" 后每 1.4 秒重启一次。
    xTaskCreate([](void* p) {
        Wifi_Station* self = static_cast<Wifi_Station*>(p);
        vTaskDelay(pdMS_TO_TICKS(200));   // 等 WiFi 启动稳定

        // 有界重试：开机第一次连接偶尔会拿到 reason=201(NO_AP_FOUND)。
        // 此时热点其实是存在的（进入配网 AP 模式后扫描能稳定看到它），
        // 属于开机瞬间 STA 首次扫描的瞬态失败。原逻辑一次失败就干等
        // 60 秒再退回配网，体验很差。这里在 60 秒超时窗口内重试 3 次。
        for (int attempt = 0; attempt < 3 && self->station_started_; attempt++) {
            if (attempt > 0) {
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            if (!self->station_started_) {
                break;
            }
            self->StartConnect();

            auto bits = xEventGroupWaitBits(
                self->event_group_,
                WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED,
                pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
            if (bits & WIFI_EVENT_CONNECTED) {
                break;                      // 已拿到 IP，收工
            }
            if (bits & WIFI_EVENT_STOPPED) {
                break;                      // 外层已停止 STA（进配网模式等）
            }
            ESP_LOGW(TAG, "Connect attempt %d/3 not connected yet, retrying", attempt + 1);
        }
        vTaskDelete(nullptr);
    }, "wifi_connect", 6144, this, 5, nullptr);

    if (on_scan_begin_) {
        on_scan_begin_();
    }
}

void Wifi_Station::StopStation() {
    station_started_ = false;
    esp_wifi_stop();
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
    ip_address_.clear();
    if (on_disconnect_) {
        on_disconnect_();
    }
}

bool Wifi_Station::IsConnected() {
    return (xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED) != 0;
}

bool Wifi_Station::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED,
                                    pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

int8_t Wifi_Station::GetRssi() {
    if (!IsConnected()) {
        return 0;
    }
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.rssi;
}

uint8_t Wifi_Station::GetChannel() {
    if (!IsConnected()) {
        return 0;
    }
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.primary;
}

void Wifi_Station::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (level == WifiPowerSaveLevel::LOW_POWER) {
        ps = WIFI_PS_MAX_MODEM;
    } else if (level == WifiPowerSaveLevel::BALANCED) {
        ps = WIFI_PS_MIN_MODEM;
    }
    esp_wifi_set_ps(ps);
}

// 从 SsidManager 取出保存的网络，配置 STA 并真正发起连接
void Wifi_Station::StartConnect() {
    if (!station_started_) {
        return;
    }

    auto ssid_list = SsidManager::GetInstance().GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGW(TAG, "No saved SSID, skip connect");
        return;
    }

    // 取第一个作为默认网络（SetDefaultSsid 会把它排到前面）
    const auto& item = ssid_list.front();
    ssid_ = item.ssid;
    password_ = item.password;

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, item.ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
    strncpy((char*)cfg.sta.password, item.password.c_str(), sizeof(cfg.sta.password) - 1);
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.failure_retry_cnt = 3;              // 认证失败时驱动内部重试次数
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return;
    }

    if (on_connect_) {
        on_connect_(ssid_);
    }

    err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_CONN) {
        // 驱动仍停留在上一次的连接过程中，先断开再发起，否则本次连接被直接忽略
        ESP_LOGW(TAG, "Still connecting, disconnect and retry");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Connecting to WiFi \"%s\"", ssid_.c_str());
    }
}

void Wifi_Station::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* self = static_cast<Wifi_Station*>(arg);
    if (self == nullptr) {
        return;
    }

    if (event_id == WIFI_EVENT_STA_START) {
        // 连接由 StartStation() 里创建的 wifi_connect 任务发起。
        // 这里绝不能再调 StartConnect()：sys_evt 任务栈很小，
        // 而且会和任务里的调用重复触发 NotifyEvent(Connecting)。
        ESP_LOGI(TAG, "WiFi station started");

    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        // 关键时序：二层（L2）握手成功不代表能用网。
        // 这里只记状态、不打 Connected 事件 —— DHCP 此时还没下发 IP，
        // 上层 WifiBoard::OnNetworkEvent(Connected) 会立刻停掉 60 秒超时定时器并
        // 通知状态机去跑 OTA / HTTP。若那时 GetIpAddress() 还是空串，
        // OTA 请求必然失败，而超时定时器已经被停掉，设备就永久卡死。
        // 因此 Connected 事件统一推迟到 IP_EVENT_STA_GOT_IP 再上报。
        self->was_connected_ = true;
        self->reconnect_count_ = 0;
        ESP_LOGI(TAG, "L2 connected to WiFi \"%s\", waiting for IP...", self->ssid_.c_str());

    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(self->event_group_, WIFI_EVENT_CONNECTED);
        self->ip_address_.clear();

        int reason = -1;
        if (event_data != nullptr) {
            reason = ((wifi_event_sta_disconnected_t*)event_data)->reason;
        }
        ESP_LOGW(TAG, "Disconnected from WiFi \"%s\", reason=%d", self->ssid_.c_str(), reason);

        // 只在"曾经连上过"的情况下上报 Disconnected，避免启动阶段的
        // 首次失败被重复上报（首次失败由外层 60 秒超时机制处理）
        if (self->was_connected_) {
            self->was_connected_ = false;
            if (self->on_disconnect_) {
                self->on_disconnect_();
            }
        }
        // 注意：这里不主动重连。重连交给驱动内部的 failure_retry_cnt，
        // 彻底失败则由 WifiBoard 的 60 秒超时切到配网模式，避免事件任务里
        // 做延时/循环重连把 WiFi 事件任务堵住。
    }
}

void Wifi_Station::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* self = static_cast<Wifi_Station*>(arg);
    if (self == nullptr || event_data == nullptr) {
        return;
    }
    auto* ev = static_cast<ip_event_got_ip_t*>(event_data);
    char buf[16] = {0};
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ev->ip_info.ip));
    self->ip_address_ = buf;
    ESP_LOGI(TAG, "Got IP address: %s", self->ip_address_.c_str());

    // 到这一步才算真正"连上网络"：先置位（让 WaitForConnected 返回），
    // 再上报 Connected（让 WifiBoard 停超时定时器 + 状态机开始 OTA）。
    xEventGroupSetBits(self->event_group_, WIFI_EVENT_CONNECTED);
    if (self->on_connected_) {
        self->on_connected_(self->ssid_);
    }
}

void Wifi_Station::HandleScanResult() {}
void Wifi_Station::UpdateScanInterval() {}
