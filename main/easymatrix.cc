#include "easymatrix.h"

#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_check.h>

#include <algorithm>
#include <cstring>

#define TAG "EasyMatrix"

/*
 * WS2812 时序（10MHz 分辨率，1 tick = 0.1us）：
 *   T0H=0.3us(3)  T0L=0.9us(9)
 *   T1H=0.9us(9)  T1L=0.3us(3)
 *   复位 >50us (500)
 */
#define WS2812_RESOLUTION_HZ 10000000

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t* bytes_encoder;
    rmt_encoder_t* copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t* encoder, rmt_channel_handle_t channel,
                            const void* primary_data, size_t data_size,
                            rmt_encode_state_t* ret_state) {
    ws2812_encoder_t* ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    uint32_t state = 0;
    size_t encoded_symbols = 0;

    switch (ws->state) {
        case 0:  // 发送 RGB 数据
            encoded_symbols += ws->bytes_encoder->encode(ws->bytes_encoder, channel,
                                                         primary_data, data_size, &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                ws->state = 1;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
            [[fallthrough]];
        case 1:  // 发送复位码
            encoded_symbols += ws->copy_encoder->encode(ws->copy_encoder, channel,
                                                        &ws->reset_code,
                                                        sizeof(ws->reset_code), &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                ws->state = RMT_ENCODING_RESET;
                state |= RMT_ENCODING_COMPLETE;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
    }
out:
    *ret_state = (rmt_encode_state_t)state;
    return encoded_symbols;
}

static esp_err_t ws2812_encoder_reset(rmt_encoder_t* encoder) {
    ws2812_encoder_t* ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws->bytes_encoder);
    rmt_encoder_reset(ws->copy_encoder);
    ws->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t ws2812_encoder_del(rmt_encoder_t* encoder) {
    ws2812_encoder_t* ws = __containerof(encoder, ws2812_encoder_t, base);
    if (ws->bytes_encoder) {
        rmt_del_encoder(ws->bytes_encoder);
    }
    if (ws->copy_encoder) {
        rmt_del_encoder(ws->copy_encoder);
    }
    free(ws);
    return ESP_OK;
}

static esp_err_t ws2812_encoder_new(rmt_encoder_handle_t* ret_encoder) {
    ws2812_encoder_t* ws = (ws2812_encoder_t*)calloc(1, sizeof(ws2812_encoder_t));
    if (ws == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ws->base.encode = ws2812_encode;
    ws->base.reset = ws2812_encoder_reset;
    ws->base.del = ws2812_encoder_del;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = {.duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0},
        .bit1 = {.duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0},
        .flags = {.msb_first = 1},
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&bytes_cfg, &ws->bytes_encoder), TAG, "new bytes encoder");

    rmt_copy_encoder_config_t copy_cfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&copy_cfg, &ws->copy_encoder), TAG, "new copy encoder");

    ws->reset_code = (rmt_symbol_word_t){
        .duration0 = 500, .level0 = 0, .duration1 = 500, .level1 = 0};
    *ret_encoder = &ws->base;
    return ESP_OK;
}

EasyMatrix::EasyMatrix(int gpio, int width, int height, bool serpentine)
    : gpio_(gpio), width_(width), height_(height), serpentine_(serpentine) {
    framebuffer_.assign((size_t)width_ * height_ * 3, 0);
}

EasyMatrix::~EasyMatrix() {
    if (encoder_) {
        rmt_del_encoder((rmt_encoder_handle_t)encoder_);
        encoder_ = nullptr;
    }
    if (rmt_handle_) {
        rmt_disable((rmt_channel_handle_t)rmt_handle_);
        rmt_del_channel((rmt_channel_handle_t)rmt_handle_);
        rmt_handle_ = nullptr;
    }
}

bool EasyMatrix::Initialize() {
    // 注意：这里绝不能持有 mutex_ 去调用 Show()。
    // Show() 内部会再次 lock mutex_，而 std::mutex 是非递归锁，
    // 同一个线程二次加锁会直接死锁（MCP 工具注册阶段就会卡死启动流程）。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) {
            return true;
        }

        rmt_tx_channel_config_t tx_cfg = {
            .gpio_num = (gpio_num_t)gpio_,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = WS2812_RESOLUTION_HZ,
            .mem_block_symbols = 64,
            .trans_queue_depth = 4,
            .intr_priority = 0,
            .flags = {.invert_out = 0, .with_dma = 0, .io_loop_back = 0,
                      .io_od_mode = 0, .allow_pd = 0},
        };
        rmt_channel_handle_t chan = nullptr;
        if (rmt_new_tx_channel(&tx_cfg, &chan) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create RMT TX channel on GPIO %d", gpio_);
            return false;
        }

        rmt_encoder_handle_t enc = nullptr;
        if (ws2812_encoder_new(&enc) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create WS2812 encoder");
            // 失败路径必须回收已创建的通道，否则 RMT 通道泄漏
            rmt_del_channel(chan);
            return false;
        }

        if (rmt_enable(chan) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable RMT channel");
            rmt_del_encoder(enc);
            rmt_del_channel(chan);
            return false;
        }

        rmt_handle_ = chan;
        encoder_ = enc;
        initialized_ = true;
        ESP_LOGI(TAG, "WS2812 matrix ready: %dx%d on GPIO %d (serpentine=%d)",
                 width_, height_, gpio_, serpentine_);
    }

    // 锁已释放，这里可以安全地走 Show() 的加锁路径
    return Show();
}

int EasyMatrix::LedIndex(int x, int y) const {
    if (x < 0) x = 0;
    if (x >= width_) x = width_ - 1;
    if (y < 0) y = 0;
    if (y >= height_) y = height_ - 1;
    if (serpentine_ && (y & 1)) {
        return y * width_ + (width_ - 1 - x);
    }
    return y * width_ + x;
}

void EasyMatrix::SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    size_t idx = (size_t)LedIndex(x, y) * 3;
    if (idx + 2 < framebuffer_.size()) {
        framebuffer_[idx] = r;
        framebuffer_[idx + 1] = g;
        framebuffer_[idx + 2] = b;
    }
}

void EasyMatrix::SetPixel(int x, int y, uint32_t rgb) {
    SetPixel(x, y, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

void EasyMatrix::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(framebuffer_.begin(), framebuffer_.end(), 0);
}

void EasyMatrix::Fill(uint8_t r, uint8_t g, uint8_t b) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i + 2 < framebuffer_.size(); i += 3) {
        framebuffer_[i] = r;
        framebuffer_[i + 1] = g;
        framebuffer_[i + 2] = b;
    }
}

bool EasyMatrix::DrawFrame(const uint8_t* rgb, size_t pixel_count) {
    if (rgb == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = (size_t)width_ * height_;
    size_t n = std::min(pixel_count, total);
    memcpy(framebuffer_.data(), rgb, n * 3);
    // 不足一帧时把余下像素清零：原实现只覆盖前 n*3 字节，
    // 残缺帧会残留上一帧的颜色，画面看起来像"花了"。
    if (n < total) {
        memset(framebuffer_.data() + n * 3, 0, (total - n) * 3);
    }
    return true;
}

static inline uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

bool EasyMatrix::DrawHexFrame(const std::string& hex) {
    size_t pixels = (size_t)width_ * height_;
    if (hex.size() < pixels * 6) {
        ESP_LOGW(TAG, "Hex frame too short: %u < %u", (unsigned)hex.size(), (unsigned)(pixels * 6));
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < pixels; i++) {
        const char* p = hex.c_str() + i * 6;
        framebuffer_[i * 3 + 0] = hex_nibble(p[0]) << 4 | hex_nibble(p[1]);
        framebuffer_[i * 3 + 1] = hex_nibble(p[2]) << 4 | hex_nibble(p[3]);
        framebuffer_[i * 3 + 2] = hex_nibble(p[4]) << 4 | hex_nibble(p[5]);
    }
    return true;
}

std::string EasyMatrix::Snapshot() const {
    // 标准十六进制字符表，按 v>>4（高位）/ v&0x0F（低位）查表。
    // 该表必须与 DrawHexFrame() 的 hex_nibble() 严格互逆，
    // 否则 get_frame 导出的字符串无法被 draw 还原。
    static const char* kHex = "0123456789ABCDEF";
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    out.reserve(framebuffer_.size() * 2);
    for (uint8_t v : framebuffer_) {
        out.push_back(kHex[(v >> 4) & 0x0F]);
        out.push_back(kHex[v & 0x0F]);
    }
    return out;
}

void EasyMatrix::SetBrightness(uint8_t brightness) {
    std::lock_guard<std::mutex> lock(mutex_);
    brightness_ = brightness;
}

bool EasyMatrix::Show() {
    if (!initialized_ || rmt_handle_ == nullptr || encoder_ == nullptr) {
        return false;
    }

    std::vector<uint8_t> grb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        grb.resize(framebuffer_.size());
        // WS2812 数据顺序为 GRB，同时叠加亮度
        for (size_t i = 0; i + 2 < framebuffer_.size(); i += 3) {
            uint8_t r = framebuffer_[i];
            uint8_t g = framebuffer_[i + 1];
            uint8_t b = framebuffer_[i + 2];
            if (brightness_ < 255) {
                r = (uint8_t)((uint16_t)r * brightness_ / 255);
                g = (uint8_t)((uint16_t)g * brightness_ / 255);
                b = (uint8_t)((uint16_t)b * brightness_ / 255);
            }
            grb[i + 0] = g;
            grb[i + 1] = r;
            grb[i + 2] = b;
        }
    }

    rmt_transmit_config_t tx_cfg = {.loop_count = 0, .flags = {.eot_level = 0}};
    if (rmt_transmit((rmt_channel_handle_t)rmt_handle_, (rmt_encoder_handle_t)encoder_,
                     grb.data(), grb.size(), &tx_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "RMT transmit failed");
        return false;
    }
    return rmt_tx_wait_all_done((rmt_channel_handle_t)rmt_handle_, 1000) == ESP_OK;
}

EasyMatrix* GetEasyMatrix() {
    // 静态局部变量的初始化由 C++11 起由编译器加锁保证只执行一次。
    // 原实现用 `static bool tried` 做标志，MCP 任务与 BLE 任务并发首调时
    // 可能双方都看到 tried==false，各自 new 一个实例并抢同一路 RMT 通道。
    static EasyMatrix* matrix = []() -> EasyMatrix* {
        auto* m = new EasyMatrix(MATRIX_GPIO, MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_SERPENTINE);
        if (!m->Initialize()) {
            ESP_LOGW(TAG, "Failed to initialize LED matrix, feature disabled");
        }
        return m;
    }();
    return matrix;
}
