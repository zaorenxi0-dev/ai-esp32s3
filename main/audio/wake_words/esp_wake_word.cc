#include "esp_wake_word.h"


#define TAG "ESPWakeWord"

ESPWakeWord::ESPWakeWord() {

}

ESPWakeWord::~ESPWakeWord() {
    if(wakenet_data_ != nullptr){
        wakenet_iface_->destroy(wakenet_data_);
        esp_srmodel_deinit(wakenet_model_);
    }
}

bool ESPWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;

    if(models_list == nullptr){
        ESP_LOGI(TAG, "Model list is null");
        wakenet_model_ = esp_srmodel_init("model");
    }
    else{
        wakenet_model_ = models_list;
    }
    
        if (wakenet_model_ == nullptr || wakenet_model_->num == -1) {
        ESP_LOGI(TAG, "Failed to initialize wakenet model");
        return false;
    }
    if(wakenet_model_->num > 1) {
        ESP_LOGI(TAG, "More than one model found, using the first one");
    } else if (wakenet_model_->num == 0) {
        ESP_LOGI(TAG, "No model found");
        return false;
    }

    char* model_name = wakenet_model_->model_name[0];
    wakenet_iface_ = (esp_wn_iface_t*)esp_wn_handle_from_name(model_name);
    wakenet_data_ = wakenet_iface_->create(model_name, DET_MODE_95);

    int audio_chunksize = wakenet_iface_->get_samp_chunksize(wakenet_data_);
    int frequency = wakenet_iface_->get_samp_rate(wakenet_data_);
    ESP_LOGI(TAG, "Wakenet model initialized, chunk size: %d, sample rate: %d", audio_chunksize, frequency);

    return true;
}

void ESPWakeWord::Start() {
    running_ = true;
}

void ESPWakeWord::Stop() {
    running_ = false;
}

size_t ESPWakeWord::GetFeedSize() {
    if (wakenet_data_ == nullptr) {
        return 0;
    }
    return wakenet_iface_->get_samp_chunksize(wakenet_data_);
}


void ESPWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;

}

void ESPWakeWord::Feed(const std::vector<int16_t>& data) {
    if(!running_ || wakenet_data_ == nullptr){
        return;
    }

    wakenet_state_t state = wakenet_iface_->detect(wakenet_data_, (int16_t*)data.data());

    if(state == WAKENET_DETECTED){
        last_detected_wake_word_ = wakenet_iface_->get_word_name(wakenet_data_, 1);
        running_ = false;
        if(wake_word_detected_callback_){
            wake_word_detected_callback_(last_detected_wake_word_);
        }
    }

}

void ESPWakeWord::EncodeWakeWordData() {
    
}

bool ESPWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    return false;
}

