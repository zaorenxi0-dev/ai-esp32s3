#pragma once

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1

#endif

#include <esp_err.h>
#include <esp_log.h>

#include <string>


class Display{
public:
    Display();
    ~Display();
    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void ClearChatMessage();
};
