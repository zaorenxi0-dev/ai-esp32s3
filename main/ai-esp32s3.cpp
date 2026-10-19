#include <stdio.h>
#include <cstdio>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // 初始化 NVS（WiFi 配置、设备设置等持久化数据的存储介质）
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 初始化默认事件循环（WiFi / IP / 协议事件依赖）
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化并运行主应用：
    //   - Initialize() 完成板级外设、音频服务、显示、MCP 等初始化
    //   - Run() 进入 FreeRTOS 事件驱动主循环，永不返回
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();
}
