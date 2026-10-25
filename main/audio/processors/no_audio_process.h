#pragma once

#include "audio_processor.h"

#include <functional>
#include <vector>

/**
 * 无音频前处理（AFE 关闭时使用）：直接在输入/输出之间透传。
 * 当 CONFIG_USE_AUDIO_PROCESSOR 未开启时作为 AudioProcessor 的默认实现。
 */
class NoAudioProcessor : public AudioProcessor {
public:
    NoAudioProcessor();
    ~NoAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

private:
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    bool running_ = false;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
};
