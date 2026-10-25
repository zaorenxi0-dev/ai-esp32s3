#include "ble.h"

#if CONFIG_BT_ENABLED

#include "easymatrix.h"

#include <cstring>

// TAG 只在本编译单元定义：原来放在 ble.h 里会让所有包含该头文件的
// 源文件都被改成 "BLE"，与各自的 TAG 冲突并触发大量宏重定义告警。
#define TAG "BLE"

const ble_uuid16_t BLE::service_uuid = BLE_UUID16_INIT(BLE_SVC_SPP_UUID16);
uint8_t BLE::device_name[] = OWN_NAME;
uint8_t BLE::ble_addr_type = 0;
EasyMatrix* BLE::WS2812 = nullptr;

uint16_t BLE::ble_gatt_handle = 0;
uint8_t BLE::rx_buf[FRAME_MAX_LEN] = {0};
uint16_t BLE::rx_len = 0;
uint16_t BLE::rx_conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
bool BLE::last_frame_complete_ = false;
static const ble_uuid16_t s_char_uuid = BLE_UUID16_INIT(BLE_SVC_SPP_CHR_UUID16);

// 帧格式: AA 55 CMD | H | W | LEN_H | LEN_L | payload(LEN) | CHECKSUM
#define FRAME_HEADER_LEN 7

BLE::BLE() {
}

BLE::~BLE() {
}

esp_err_t BLE::init() {
    // 复用全局 LED 矩阵实例（GPIO / 尺寸由 easymatrix.h 中的宏配置）
    BLE::WS2812 = GetEasyMatrix();
    if (BLE::WS2812 == nullptr) {
        ESP_LOGE(TAG, "LED matrix unavailable, BLE pixel-art disabled");
        return ESP_FAIL;
    }
    if (!BLE::WS2812->initialized()) {
        BLE::WS2812->Initialize();
    }

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed, err: %d", ret);
        return ret;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 1;

    if (ble_svc_gap_device_name_set(OWN_NAME) != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed");
        return ESP_FAIL;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // 计算自定义服务需要的资源数量
    ret = ble_gatts_count_cfg(ble_svc_gatt_defs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config GATT service %d", ret);
        return ret;
    }
    // 将自定义服务添加到 GATT 数据库
    ret = ble_gatts_add_svcs(ble_svc_gatt_defs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add GATT service %d", ret);
        return ret;
    }

    // 启动 NimBLE Host 任务
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE pixel-art service started, name=%s", OWN_NAME);
    return ESP_OK;
}

void BLE::ble_host_task(void* param) {
    // 运行 NimBLE 协议栈事件循环（阻塞）
    nimble_port_run();
    nimble_port_freertos_deinit();
}

const struct ble_gatt_svc_def BLE::ble_svc_gatt_defs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (const ble_uuid_t*)&BLE::service_uuid,
        .characteristics = (ble_gatt_chr_def[]){
            {
                .uuid = (const ble_uuid_t*)&s_char_uuid,
                .access_cb = BLE::ble_svc_gatt_handler,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &BLE::ble_gatt_handle,
            },
            {
                0,
            }
        },
    },
    {
        0,
    },
};

int BLE::ble_svc_gatt_handler(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt* ctxt, void* arg) {
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            ESP_LOGI(TAG, "Callback for read");
            break;
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            ESP_LOGI(TAG, "Data received in write event,conn_handle = %x,attr_handle = %x", conn_handle, attr_handle);
            int rc = BLE::gatt_svr_chr_write(conn_handle, attr_handle, ctxt, arg);
            if (rc != 0) {
                // 负数/非法码不能作为 ATT 状态返回，统一转成标准错误码
                return (rc > 0 && rc < 0x100) ? rc : BLE_ATT_ERR_UNLIKELY;
            }

            // 只在“整帧收完并解析”后回一条 Notify。
            // 原实现在每个 BLE 分包后都回包，一帧 8x8 会被拆成约 10 包，
            // 等于连发 10 条通知，既浪费带宽也容易打满 mbuf。
            if (!BLE::last_frame_complete_) {
                break;
            }
            BLE::last_frame_complete_ = false;

            const char* reply = "I have receive your message";
            struct os_mbuf* om = ble_hs_mbuf_from_flat(reply, strlen(reply));
            if (om == nullptr) {
                ESP_LOGE(TAG, "Failed to alloc os_mbuf");
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            ble_gatts_notify_custom(conn_handle, ble_gatt_handle, om);
            break;
        }
        default:
            break;
    }
    return 0;
}

// 蓝牙写特征回调函数：分包重组 + 整帧解析
int BLE::gatt_svr_chr_write(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)attr_handle;
    (void)arg;

    // 1. 获取当前收到的小包长度
    uint16_t chunk_len = OS_MBUF_PKTLEN(ctxt->om);
    if (chunk_len == 0) {
        return 0;
    }
    if (chunk_len > FRAME_MAX_LEN) {
        ESP_LOGW(TAG, "chunk too large (%u), dropped", chunk_len);
        return 0;
    }

    // 2. 只取前两字节判断帧头。
    //    原实现在 NimBLE host 任务栈上开 uint8_t chunk_data[1024] 再整体拷贝，
    //    而 host 任务栈只有 CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE（默认 4096B），
    //    叠加 ESP_LOG 与协议栈自身开销后余量很小；这里改成"先探帧头、
    //    再直接搬进重组缓存"，不再占用栈空间。
    uint8_t head[2] = {0};
    bool has_head = (chunk_len >= 2);
    if (has_head) {
        if (ble_hs_mbuf_to_flat(ctxt->om, head, sizeof(head), NULL) != 0) {
            ESP_LOGE(TAG, "mbuf_to_flat(head) failed");
            return BLE_ATT_ERR_UNLIKELY;
        }
    }

    // 3. 收到 AA 55 帧头，或换了连接，都视为新一帧开始，重置缓存。
    //    多连接场景下必须按连接隔离，否则两路中心设备的分包会交错拼接。
    if (conn_handle != BLE::rx_conn_handle_ ||
        (has_head && head[0] == 0xAA && head[1] == 0x55)) {
        BLE::rx_len = 0;
        BLE::rx_conn_handle_ = conn_handle;
    }

    // 4. 将小包追加到全局大缓存中
    if ((size_t)BLE::rx_len + chunk_len > FRAME_MAX_LEN) {
        ESP_LOGW(TAG, "接收缓冲区溢出，丢弃");
        BLE::rx_len = 0;
        return 0;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, &BLE::rx_buf[BLE::rx_len], chunk_len, NULL) != 0) {
        ESP_LOGE(TAG, "mbuf_to_flat(payload) failed");
        BLE::rx_len = 0;
        return BLE_ATT_ERR_UNLIKELY;
    }
    BLE::rx_len += chunk_len;

    // 5. 判定整帧是否接收完毕（帧头 7 字节含负载长度）
    if (BLE::rx_len >= FRAME_HEADER_LEN) {
        uint16_t payload_len = ((uint16_t)BLE::rx_buf[5] << 8) | BLE::rx_buf[6];
        uint16_t expected = FRAME_HEADER_LEN + payload_len + 1;  // 负载 + 校验字节
        if (expected > FRAME_MAX_LEN) {
            ESP_LOGW(TAG, "非法帧长度 %d，丢弃", expected);
            BLE::rx_len = 0;
            return 0;
        }
        if (BLE::rx_len >= expected) {
            ESP_LOGI(TAG, ">>> %u 字节完整帧接收完毕，开始解析！ <<<", expected);
            // 只有解析成功才回 Notify；畸形帧不该让对端以为绘制已生效
            BLE::last_frame_complete_ = BLE::parse_and_draw_rgb_frame(BLE::rx_buf, expected);
            BLE::rx_len = 0;  // 解析完清空，等待下一帧
        }
    }

    return 0;
}

/**
 * @brief 解析完整帧并点亮矩阵
 * @param frame 完整帧
 * @param frame_len 帧长度
 * @return true 成功解析并点亮，false 解析失败
 */
bool BLE::parse_and_draw_rgb_frame(const uint8_t* frame, size_t frame_len) {
    if (BLE::WS2812 == nullptr) {
        return false;
    }

    // 1. 基础长度校验
    if (frame_len < FRAME_HEADER_LEN + 1) {
        ESP_LOGE(TAG, "Frame too short: %d", (int)frame_len);
        return false;
    }

    // 2. 包头和命令字校验 (AA 55 02)
    if (frame[0] != 0xAA || frame[1] != 0x55 || frame[2] != 0x02) {
        ESP_LOGE(TAG, "Header or command error: %02X %02X %02X", frame[0], frame[1], frame[2]);
        return false;
    }

    // 3. 宽高与负载长度校验
    uint8_t fh = frame[3];
    uint8_t fw = frame[4];
    if (fh == 0 || fw == 0) {
        ESP_LOGE(TAG, "Invalid frame size: H=%u W=%u", fh, fw);
        return false;
    }
    uint16_t payload_len = ((uint16_t)frame[5] << 8) | frame[6];
    // H/W 各 1 字节，最大 255*255*3 = 195075，远超 uint16 可表示范围；
    // 先按 32 位算出需求长度再比较，避免截断后"碰巧相等"放过畸形帧。
    uint32_t need = (uint32_t)fh * fw * 3;
    if (need > 0xFFFF || payload_len != need) {
        ESP_LOGE(TAG, "Payload length error: %u (H=%u W=%u, need %u)",
                 payload_len, fh, fw, (unsigned)need);
        return false;
    }
    if ((size_t)FRAME_HEADER_LEN + payload_len + 1 > frame_len) {
        ESP_LOGE(TAG, "Frame truncated");
        return false;
    }

    // 4. 校验和校验 (从 byte[2] 累加到 payload 末尾，与最后一字节对比)
    uint8_t calculated_checksum = 0;
    for (size_t i = 2; i < FRAME_HEADER_LEN + payload_len; i++) {
        calculated_checksum += frame[i];
    }
    if (calculated_checksum != frame[FRAME_HEADER_LEN + payload_len]) {
        ESP_LOGE(TAG, "Checksum error: expected %02X, calculated %02X",
                 frame[FRAME_HEADER_LEN + payload_len], calculated_checksum);
        return false;
    }

    ESP_LOGI(TAG, "Frame parsed successfully, updating matrix...");

    // 5. 提取 RGB 并点亮（按行优先，越界裁剪到本机矩阵尺寸）
    const uint8_t* payload = &frame[FRAME_HEADER_LEN];
    auto* m = BLE::WS2812;
    int mw = m->width();
    int mh = m->height();

    m->Clear();
    for (int y = 0; y < fh && y < mh; y++) {
        for (int x = 0; x < fw && x < mw; x++) {
            int idx = (y * fw + x) * 3;
            m->SetPixel(x, y, payload[idx], payload[idx + 1], payload[idx + 2]);
        }
    }
    m->Show();
    return true;
}

void BLE::ble_on_reset(int reason) {
    ESP_LOGI(TAG, "ble_on_reset, reason: %d", reason);
}

void BLE::ble_on_sync(void) {
    int rc;
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed, err: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &ble_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed, err: %d", rc);
        return;
    }
    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(ble_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_copy_addr failed, err: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "Device address: %02X:%02X:%02X:%02X:%02X:%02X",
             addr_val[5], addr_val[4], addr_val[3],
             addr_val[2], addr_val[1], addr_val[0]);

    /* 使能广播 */
    ble_advertise();
}

void BLE::ble_advertise(void) {
    int rc;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.name = device_name;
    fields.name_len = strlen(OWN_NAME);
    fields.name_is_complete = 1;

    fields.uuids16 = &BLE::service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed, err: %d", rc);
        return;
    }

    ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER, &adv_params, BLE::ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed, err: %d", rc);
        return;
    }
}

int BLE::ble_gap_event(struct ble_gap_event* event, void* arg) {
    int rc = 0;

    switch (event->type) {
        case BLE_GAP_EVENT_LINK_ESTAB:
            ESP_LOGI(TAG, "connection %s, status=%d",
                     event->connect.status == 0 ? "established" : "failed",
                     event->connect.status);
            if (event->connect.status != 0) {
                BLE::ble_advertise();
            } else {
                struct ble_gap_conn_desc desc;
                rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
                if (rc != 0) {
                    // GAP 事件回调的返回值不参与 ATT 状态，这里不能直接 return rc，
                    // 否则会把 NimBLE 内部错误码当成事件处理结果往上抛。
                    ESP_LOGE(TAG, "ble_gap_conn_find failed, err: %d", rc);
                }
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnect, reason=%d", event->disconnect.reason);
            BLE::ble_advertise();
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(TAG, "advertise complete");
            ble_advertise();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "subscribe event, cur_notify=%d, val_handle=%d",
                     event->subscribe.cur_notify,
                     event->subscribe.attr_handle);
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "mtu updated, conn_handle=%d mtu=%d",
                     event->mtu.conn_handle,
                     event->mtu.value);
            break;

        default:
            break;
    }
    return rc;
}

#endif  // CONFIG_BT_ENABLED
