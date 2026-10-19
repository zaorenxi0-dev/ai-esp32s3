#pragma once

#include <esp_err.h>
#include <esp_log.h>

#include <string>
#include <vector>
#include "nvs_flash.h"

struct SsidItem {
    std::string ssid;
    std::string password;
};


class SsidManager{
public:
    static SsidManager& GetInstance() {
        static SsidManager instance;
        return instance;
    }

    void AddSsid(const std::string& ssid, const std::string& password);
    std::vector<SsidItem> GetSsidList() const{ return ssid_list_; };
    void RemoveSsid(int index);
    void SetDefaultSsid(int index);
private:
    SsidManager();
    ~SsidManager();
    std::vector<SsidItem> ssid_list_;
    void SaveToNvs();
    void LoadFromNvs();
};