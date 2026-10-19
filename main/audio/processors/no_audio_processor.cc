#include "no_audio_process.h"

#include <esp_log.h>

#define TAG "NoAudioProcessor"

NoAudioProcessor::NoAudioProcessor() {
}

NoAudioProcessor::~NoAudioProcessor() {
}

void NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    (void)models_list;
    codec_ = codec;
    const int sample_rate = codec_ ? codec_->input_sample_rate() : 16000;
    frame_samples_ = frame_duration_ms * sample_rate / 1000;
}

void NoAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (!running_) {
        return;
    }
    // 透传：无 VAD/AEC 时认为始终在说话，直接输出给编码队列
    if (vad_state_change_callback_) {
        vad_state_change_callback_(true);
    }
    if (output_callback_) {
        output_callback_(std::move(data));
    }
}

void NoAudioProcessor::Start() {
    running_ = true;
}

void NoAudioProcessor::Stop() {
    running_ = false;
}

bool NoAudioProcessor::IsRunning() {
    return running_;
}

void NoAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = std::move(callback);
}

void NoAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = std::move(callback);
}

size_t NoAudioProcessor::GetFeedSize() {
    return frame_samples_;
}

void NoAudioProcessor::EnableDeviceAec(bool enable) {
    (void)enable;
    ESP_LOGI(TAG, "Device AEC is not supported by NoAudioProcessor");
}
