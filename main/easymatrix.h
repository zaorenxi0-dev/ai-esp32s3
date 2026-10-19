#pragma once

#include <esp_err.h>
#include <esp_log.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// 默认矩阵硬件配置（可通过编译宏覆盖）
#ifndef MATRIX_GPIO
#define MATRIX_GPIO 10
#endif
#ifndef MATRIX_WIDTH
#define MATRIX_WIDTH 8
#endif
#ifndef MATRIX_HEIGHT
#define MATRIX_HEIGHT 8
#endif
#ifndef MATRIX_SERPENTINE
#define MATRIX_SERPENTINE 1
#endif

/**
 * EasyMatrix —— WS2812 / WS2812B 灯珠矩阵驱动
 *
 * 基于 ESP-IDF 新版 RMT TX 驱动 + 自定义 WS2812 编码器实现，无需依赖外部
 * led_strip 组件。支持逐像素绘制、整帧刷新、亮度和 "蛇形走线" 面板映射，
 * 供 MCP 像素绘画工具与 BLE / 微信小程序控制通道调用。
 */
class EasyMatrix {
public:
    /**
     * @param gpio       WS2812 数据线所接 GPIO
     * @param width      矩阵宽度（列数）
     * @param height     矩阵高度（行数）
     * @param serpentine 面板是否为蛇形（逐行交替方向）走线
     */
    EasyMatrix(int gpio = MATRIX_GPIO, int width = MATRIX_WIDTH,
               int height = MATRIX_HEIGHT, bool serpentine = MATRIX_SERPENTINE);
    ~EasyMatrix();

    bool Initialize();
    bool initialized() const { return initialized_; }

    int width() const { return width_; }
    int height() const { return height_; }

    // ---- 像素级操作（坐标原点在左上角） ---------------------------------
    void SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
    void SetPixel(int x, int y, uint32_t rgb);          // 0xRRGGBB
    void Clear();
    void Fill(uint8_t r, uint8_t g, uint8_t b);

    /// 以 RGB888（width*height*3 字节，行优先）整帧刷新
    bool DrawFrame(const uint8_t* rgb, size_t pixel_count);
    /// 传入 "RRGGBB" hex 字符串（长度 = width*height*6）整帧刷新
    bool DrawHexFrame(const std::string& hex);

    /// 把当前帧导出为 "RRGGBB..." hex 字符串
    std::string Snapshot() const;

    void SetBrightness(uint8_t brightness);             // 0-255
    uint8_t brightness() const { return brightness_; }

    /// 把内部帧缓冲推送到灯珠
    bool Show();

private:
    int gpio_;
    int width_;
    int height_;
    bool serpentine_;
    bool initialized_ = false;
    uint8_t brightness_ = 40;

    mutable std::mutex mutex_;
    std::vector<uint8_t> framebuffer_;   // RGB888, 逻辑像素顺序

    void* rmt_handle_ = nullptr;         // rmt_channel_handle_t
    void* encoder_ = nullptr;            // rmt_encoder_handle_t
    const void* copy_encoder_ = nullptr; // rmt_encoder_handle_t

    /// 逻辑坐标 -> 物理灯珠序号（含蛇形映射）
    int LedIndex(int x, int y) const;
};

/// 获取全局 LED 矩阵实例（首次调用时按默认配置初始化，失败返回 nullptr）
EasyMatrix* GetEasyMatrix();
