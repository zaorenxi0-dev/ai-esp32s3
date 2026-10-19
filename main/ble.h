#pragma once

#include <string>
#include <esp_err.h>
#include <esp_log.h>

#include "sdkconfig.h"

#if CONFIG_BT_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

class EasyMatrix;

#define OWN_NAME "ESP32-Matrix"
#define BLE_SVC_SPP_UUID16       0xABF0
#define BLE_SVC_SPP_CHR_UUID16   0xABF1
#define FRAME_MAX_LEN            1024

/**
 * BLE —— 蓝牙低功耗像素绘画控制通道
 *
 * 通过自定义 GATT 服务（0xABF0 / 0xABF1）接收微信小程序发来的 RGB 帧，
 * 分包重组后解析并驱动 WS2812 灯珠矩阵（EasyMatrix）。
 * 仅当 CONFIG_BT_ENABLED 时才参与编译。
 */
class BLE {
public:
    BLE();
    ~BLE();

    esp_err_t init();

private:
    static EasyMatrix* WS2812;
    esp_err_t ret;
    static uint8_t ble_addr_type;
    static uint8_t device_name[];
    static const struct ble_gatt_svc_def ble_svc_gatt_defs[];
    static uint16_t ble_gatt_handle;
    static const ble_uuid16_t service_uuid;
    static uint8_t rx_buf[FRAME_MAX_LEN];
    static uint16_t rx_len;
    /// 重组缓存当前归属的连接句柄（CONFIG_BT_NIMBLE_MAX_CONNECTIONS > 1 时
    /// 防止两路中心设备的分包交错拼进同一个缓冲区）
    static uint16_t rx_conn_handle_;
    /// 本次 write 是否刚好拼完一整帧（用于只在整帧结束时回一条 Notify）
    static bool last_frame_complete_;

    static void ble_on_reset(int reason);
    static void ble_on_sync(void);
    static void ble_advertise(void);
    static int ble_gap_event(struct ble_gap_event* event, void* arg);
    static int ble_svc_gatt_handler(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt* ctxt, void* arg);
    static void ble_host_task(void* param);
    // 蓝牙写特征回调函数
    static int gatt_svr_chr_write(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt* ctxt, void* arg);
    static bool parse_and_draw_rgb_frame(const uint8_t* frame, size_t frame_len);
};

#endif  // CONFIG_BT_ENABLED
