#pragma once

#include <esp_err.h>
#include <esp_log.h>

#include <string>
#include <deque>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>


#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "audio_codec.h"
#include "wake_word.h"


#include "custom_wake_word.h"
#include "audio_service.h"
#include "system_info.h"
#include "assets.h"

#include <esp_afe_sr_models.h>
#include <esp_nsn_models.h>
#include <model_path.h>
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"

class AFEWakeWord : public WakeWord {
public:
    AFEWakeWord();
    ~AFEWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    void Feed(const std::vector<int16_t>& data) override;
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const { return last_detected_wake_word_; }

private:
    AudioCodec* codec_;
    srmodel_list_t* model_ = nullptr;
    std::string last_detected_wake_word_;
    EventGroupHandle_t event_group_;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    char*wakenet_model_name_ = nullptr;
    std::vector<std::string> wake_words_;
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;

    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
    std::deque<std::vector<int16_t>> wake_word_pcm_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;

    void StoreWakeWordData(const int16_t* data, size_t samples);
    void AudioDetectionTask();
};