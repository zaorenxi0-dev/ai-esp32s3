#include "ssid_manager.h"

#define NVS_NAMESPACE "wifi"
#define MAX_WIFI_SSID_COUNT 10
#define TAG "SsidManager"

SsidManager::SsidManager()
{
    LoadFromNvs();
}

SsidManager::~SsidManager()
{
}

void SsidManager::LoadFromNvs()
{
    ssid_list_.clear();
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Failed to open NVS handle: %s", esp_err_to_name(err));
        return;
    }

    for(int i = 0; i < MAX_WIFI_SSID_COUNT; i++){
        std::string ssid_key = "ssid_" + std::to_string(i);
        std::string password_key = "password_" + std::to_string(i);
        char ssid[33] = {0};
        char password[65] = {0};
        size_t ssid_len = sizeof(ssid);
        size_t password_len = sizeof(password);
        err = nvs_get_str(nvs_handle, ssid_key.c_str(), ssid, &ssid_len);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "Failed to get SSID from NVS: %s", esp_err_to_name(err));
            continue;
        }
        err = nvs_get_str(nvs_handle, password_key.c_str(), password, &password_len);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "Failed to get password from NVS: %s", esp_err_to_name(err));
            continue;
        }
        SsidItem item;
        item.ssid = ssid;
        item.password = password;
        ssid_list_.push_back(item);
    }
    nvs_close(nvs_handle);
}
void SsidManager::SaveToNvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Failed to open NVS handle: %s", esp_err_to_name(err));
        return;
    }
    for(int i = 0; i < MAX_WIFI_SSID_COUNT; i++){
        std::string ssid_key = "ssid_" + std::to_string(i);
        std::string password_key = "password_" + std::to_string(i);
       if (i < ssid_list_.size()) {
            nvs_set_str(nvs_handle, ssid_key.c_str(), ssid_list_[i].ssid.c_str());
            nvs_set_str(nvs_handle, password_key.c_str(), ssid_list_[i].password.c_str());
        } else {
            nvs_erase_key(nvs_handle, ssid_key.c_str());
            nvs_erase_key(nvs_handle, password_key.c_str());
        }
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

void SsidManager::AddSsid(const std::string& ssid, const std::string& password)
{
    for(auto& item: ssid_list_){
        if(item.ssid == ssid){
            ESP_LOGI(TAG, "SSID %s already exists", ssid.c_str());
            item.password = password;
            SaveToNvs();
            return;
        }
    }

    if (ssid_list_.size() >= MAX_WIFI_SSID_COUNT) {
    ESP_LOGI(TAG, "SSID list is full, pop one");
    ssid_list_.pop_back();
    }
    // Add the new ssid to the front of the list
    ssid_list_.insert(ssid_list_.begin(), {ssid, password});
    SaveToNvs();
}

void SsidManager::RemoveSsid(int index)
{
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGI(TAG, "Invalid index %d", index);
        return;
    }
    ssid_list_.erase(ssid_list_.begin() + index);
    SaveToNvs();
}

void SsidManager::SetDefaultSsid(int index)
{
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGI(TAG, "Invalid index %d", index);
        return;
    }
    // Move the selected ssid to the front of the list
    SsidItem selected = ssid_list_[index];
    ssid_list_.erase(ssid_list_.begin() + index); 
    ssid_list_.insert(ssid_list_.begin(), selected);
    SaveToNvs();
}
