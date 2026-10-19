#include "audio_codec.h"
#include <esp_log.h>
#include "setting.h"


AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data){
    Write((const int16_t*)data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data){
    int samples_read = Read((int16_t*)data.data(), data.size());
    return samples_read > 0;
}

void AudioCodec::start() {
    Setting setting("audio_codec", false);
    output_volume_ = setting.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGI(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    if (i2s_tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_handle_));
    }

    if (i2s_rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_handle_));
    }

    EnableInput(true);
    EnableOutput(true);
    ESP_LOGI(TAG, "Audio codec started");

}

void AudioCodec::SetOutputVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    output_volume_ = volume;
    Setting setting("audio_codec", true);
    setting.SetInt("output_volume", output_volume_);
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
}

void AudioCodec::SetInputGain(float gain){    input_gain_=gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}
void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}


