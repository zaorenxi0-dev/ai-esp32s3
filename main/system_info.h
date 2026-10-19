#pragma once

#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <esp_mac.h>
#include <esp_flash.h>
#include <esp_app_desc.h>



class SystemInfo {
public:
    static void PrintHeapStats();
    static std::string GetUserAgent();
    static std::string GetMacAddress();
    static std::string GetChipModelName();
    static size_t GetFlashSize();
    static size_t GetMinimumFreeHeapSize();
    static size_t GetFreeHeapSize();
};



