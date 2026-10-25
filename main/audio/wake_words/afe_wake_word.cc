#include "afe_wake_word.h"

#define DETECTION_RUNNING_EVENT 1
#define TAG "AFEWakeWord"

AFEWakeWord::AFEWakeWord() 
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_(){
    event_group_ = xEventGroupCreate();
}

AFEWakeWord::~AFEWakeWord() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (model_ != nullptr) {
        esp_srmodel_deinit(model_);
    }

    vEventGroupDelete(event_group_);
}

bool AFEWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;

    if(models_list == nullptr){
        ESP_LOGI(TAG, "Model list is null");
        model_ = esp_srmodel_init("model");
    }
    else{
        model_ = models_list;
    }
    
        if (model_ == nullptr || model_->num == -1) {
        ESP_LOGI(TAG, "Failed to initialize wakenet model");
        return false;
    }
    for (int i=0; i<model_->num; i++) {
        if (strstr(model_->model_name[i], ESP_WN_PREFIX) != NULL) {
           wakenet_model_name_ = model_->model_name[i];
           auto words = esp_srmodel_get_wake_words(model_, wakenet_model_name_);
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                wake_words_.push_back(word);
            }
        }
    }

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    afe_config_t *afe_config = afe_config_init(input_format.c_str(), model_, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

    xTaskCreate([](void* arg){
        auto this_=static_cast<AFEWakeWord*>(arg);
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
    },"audio_detection",4096,this,3,nullptr);

    return true;
}

void AFEWakeWord::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdFALSE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;;
        }
        StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

        if (res->wakeup_state == WAKENET_DETECTED) {
            Stop();
            last_detected_wake_word_ = wake_words_[res->wake_word_index - 1];

            if(wake_word_detected_callback_){
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }
}

void AFEWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {

    // store audio data to wake_word_pcm_
    wake_word_pcm_.emplace_back(std::vector<int16_t>(data, data + samples));
    // keep about 2 seconds of data, detect duration is 30ms (sample_rate == 16000, chunksize == 512)
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}


void AFEWakeWord::Start() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AFEWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}
    

size_t AFEWakeWord::GetFeedSize() {
    if(afe_iface_ == nullptr || afe_data_ == nullptr){
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}


void AFEWakeWord::Feed(const std::vector<int16_t>& data) {
    if(afe_data_ == nullptr || afe_iface_ == nullptr){
        return;
    }
    afe_iface_->feed(afe_data_, (int16_t*)data.data());
}

void AFEWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AFEWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096;
    wake_word_opus_.clear();
    if(wake_word_encode_task_stack_ == nullptr){
       wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
       assert(wake_word_encode_task_stack_ != nullptr);      
    }

    if(wake_word_encode_task_buffer_ == nullptr){
       wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
       assert(wake_word_encode_task_buffer_ != nullptr);      
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg){
        auto this_ = static_cast<AFEWakeWord*>(arg);
        {
            auto start_time = esp_timer_get_time();

            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);         
            if(encoder_handle == nullptr){
                ESP_LOGI(TAG, "Failed to create audio encoder, error code: %d", ret);
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>()); // push an empty vector to indicate failure
                this_->wake_word_cv_.notify_one();
            }

            int frame_size = 0;
            int out_buf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &out_buf_size);
            frame_size = frame_size / sizeof(int16_t);

            int packets=0;
            std::vector<int16_t> in_buffer;            
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};

            for(auto& pcm : this_->wake_word_pcm_){
                if (in_buffer.empty()) {
                    in_buffer = std::move(pcm);
                } else {
                    in_buffer.reserve(in_buffer.size() + pcm.size());
                    in_buffer.insert(in_buffer.end(), pcm.begin(), pcm.end());
                }
                while(in_buffer.size()>=frame_size){
                    std::vector<uint8_t> opus_buf(out_buf_size);                    
                    in.buffer = (uint8_t *)(in_buffer.data());
                    in.len = (uint32_t)(frame_size * sizeof(int16_t));
                    out.buffer = opus_buf.data();
                    out.len = out_buf_size;
                    out.encoded_bytes = 0;

                    ret = esp_opus_enc_process(encoder_handle, &in, &out);
                    if(ret == ESP_AUDIO_ERR_OK){
                        std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                        this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                        this_->wake_word_cv_.notify_all();
                        packets++;
                    } else {
                        ESP_LOGI(TAG, "Failed to encode wake word data, error code: %d", ret);
                    }
                    in_buffer.erase(in_buffer.begin(), in_buffer.begin() + frame_size);
                }
            }
            this_->wake_word_pcm_.clear();
            esp_opus_enc_close(encoder_handle);
            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Finished encoding wake word data, total packets: %d, time cost: %lld ms", packets, (end_time - start_time) / 1000);

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();            
        }
        vTaskDelete(NULL);
    },"wake_word_encode_task", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);


}
bool AFEWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this](){ 
        return !wake_word_opus_.empty(); 
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();

}


