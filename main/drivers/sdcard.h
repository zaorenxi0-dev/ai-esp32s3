#pragma once
#include <esp_err.h>

class SDCard {
public:
    SDCard();
    ~SDCard();

    esp_err_t mount(const char* mount_point);
    esp_err_t unmount();
private:
};