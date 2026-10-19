#pragma once

#include <string>
#include <nvs_flash.h>

class Setting
{
private:
    /* data */
    std::string ns_;
    nvs_handle_t nvs_handle_=0;
    bool read_write_;
    bool dirty_=false;
public:
    Setting(const std::string& ns,bool read_write = false);
    ~Setting();

    std::string GetString(const std::string& key,const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key,int32_t default_value);
    void SetInt(const std::string& key,int32_t value);
    bool GetBool(const std::string& key,bool default_value);
    void SetBool(const std::string& key, bool value);
    void EraseKey(const std::string& key);
    void EraseAll();




};




