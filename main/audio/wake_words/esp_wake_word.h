#pragma once

#include <esp_err.h>
#include <esp_log.h>

#include <wake_word.h>
#include <vector>
#include <functional>
#include <atomic>
#include <string>

#include "audio_codec.h"
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"

class ESPWakeWord : public WakeWord {

public:
    ESPWakeWord();
    ~ESPWakeWord();

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
  srmodel_list_t* wakenet_model_ = nullptr;
  esp_wn_iface_t* wakenet_iface_ = nullptr;
  model_iface_data_t* wakenet_data_ = nullptr;
  std::atomic<bool> running_ = false;
  std::string last_detected_wake_word_;
  std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
};