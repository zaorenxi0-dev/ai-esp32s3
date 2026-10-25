#include "display.h"

#define TAG "Display"

Display::Display() {
}

Display::~Display() {
}

void Display::SetStatus(const char* status) {
    ESP_LOGI(TAG, "Status: %s", status ? status : "");
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    (void)duration_ms;
    ESP_LOGI(TAG, "Notification: %s", notification ? notification : "");
}

void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

void Display::ClearChatMessage() {
    ESP_LOGD(TAG, "Clear chat message");
}
