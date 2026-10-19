#include "setting.h"
#include "esp_log.h"
static const char* TAG = "Setting";

Setting::Setting(const std::string& ns,bool read_write):ns_(ns),read_write_(read_write){
    nvs_flash_init();
    nvs_open(ns_.c_str(), read_write ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
}

Setting::~Setting() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            nvs_commit(nvs_handle_);
        }
        nvs_close(nvs_handle_);
        nvs_handle_ = 0;
    }
}

std::string Setting::GetString(const std::string& key,const std::string& default_value){
    if(nvs_handle_ == 0){
        return default_value;
    }

    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }//获取数据长度

    std::string value;
    value.resize(length);//调整数据大小
    ESP_ERROR_CHECK(nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length));
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }//清空末尾的空字符
    return value;
}

void Setting::SetString(const std::string& key, const std::string& value) {
    if (read_write_) {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handle_, key.c_str(), value.c_str()));
        dirty_ = true;
    } else {
        ESP_LOGI(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

int32_t Setting::GetInt(const std::string& key, int32_t default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    int32_t value;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

void Setting::SetInt(const std::string& key, int32_t value) {
    if (read_write_) {
        ESP_ERROR_CHECK(nvs_set_i32(nvs_handle_, key.c_str(), value));
        dirty_ = true;
    } else {
        ESP_LOGI(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

bool Setting::GetBool(const std::string& key, bool default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    uint8_t value;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

void Setting::SetBool(const std::string& key, bool value) {
    if (read_write_) {
        ESP_ERROR_CHECK(nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0));
        dirty_ = true;
    } else {
        ESP_LOGI(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Setting::EraseKey(const std::string& key) {//删除指定键值
    if (read_write_) {
        auto ret = nvs_erase_key(nvs_handle_, key.c_str());
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(ret);
        }
    } else {
        ESP_LOGI(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Setting::EraseAll() {//删除所有键值
    if (read_write_) {
        ESP_ERROR_CHECK(nvs_erase_all(nvs_handle_));
    } else {
        ESP_LOGI(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}




