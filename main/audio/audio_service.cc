#include "audio_service.h"
#include <cstring>

#define TAG "AudioService"

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate,_frame_duration_ms)       \
    (esp_opus_dec_cfg_t) \
    { \
        .sample_rate = (uint32_t)(_sample_rate), \
        .channel = ESP_AUDIO_MONO, \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms), \
        .self_delimited = false, \
    }

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_process.h"
#endif



AudioService::AudioService(){
    audio_event_group_ = xEventGroupCreate();

}

AudioService::~AudioService() {
    if (audio_event_group_ != nullptr) {
        vEventGroupDelete(audio_event_group_);
        audio_event_group_ = nullptr;
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
        opus_encoder_ = nullptr;
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }
}


void AudioService::Initialize(AudioCodec* codec){
    codec_ = codec;
    codec_->start();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec_->input_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if(opus_decoder_==nullptr){
        ESP_LOGI(TAG,"Fail to create audio decoder,error code:%d",ret);
    }
    else{   
        decoder_sample_rate_=codec->output_sample_rate();
        decoder_duration_ms_=OPUS_FRAME_DURATION_MS;
        decoder_frame_size=decoder_sample_rate_/1000*OPUS_FRAME_DURATION_MS;
    }

    esp_opus_enc_config_t opus_enc_cfg=AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg,sizeof(esp_opus_enc_config_t),&opus_encoder_);
    if(opus_encoder_==nullptr){
        ESP_LOGI("TAG","Fail to create audio encoder,error code:%d",ret);
    }
    else{
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ =encoder_frame_size_/sizeof(int16_t);
    }
#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_=std::make_unique<AFEAudioProcessor>();
#else
    audio_processor_=std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data){
       PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue,std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking){
        voice_detected_ = speaking;
        if(callbacks_.on_vad_change){
            callbacks_.on_vad_change(speaking);
        }
    });
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg){
            auto* this_ = static_cast<AudioService*>(arg);
            this_->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &audio_power_timer_);

}

void AudioService::Start(){
    service_stopped_ = false;

    xEventGroupClearBits(audio_event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_TESTING_RUNNING);
    
    esp_timer_start_periodic(audio_power_timer_, 1000000);//启动音频电源定时器，周期为1秒
#if CONFIG_USE_AUDIO_PROCESSOR
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();//音频输入任务
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();//音频输出任务
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();//音频输入任务
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

#endif
        /* Start the opus codec task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();//opus编解码任务
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_);


}
void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(audio_event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}
void AudioService::CheckAndUpdateAudioPowerState(){
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
    if(input_elapsed > AUDIO_POWER_TIMEOUT_MS&&codec_->input_enabled()){
        codec_->EnableInput(false);
        ESP_LOGI(TAG,"Audio input disabled to save power");
    }
    if(output_elapsed > AUDIO_POWER_TIMEOUT_MS&&codec_->output_enabled()){
        codec_->EnableOutput(false);
        ESP_LOGI(TAG,"Audio output disabled to save power");
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::AudioInputTask(){
    while (true)
        {
         EventBits_t bits =xEventGroupWaitBits(audio_event_group_, AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING, pdFALSE, pdFALSE, portMAX_DELAY);

        if(service_stopped_){
            break;
        }

        if(audio_input_need_warmup_){
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if(bits & AS_EVENT_AUDIO_TESTING_RUNNING){
                // 音频回环自测：把处理后的 PCM 编码后再解码，验证整条编解码链路
                std::vector<int16_t> data;
                int samples = audio_processor_->GetFeedSize();
                if(samples > 0 && ReadAudioData(data, 16000, samples)){
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
        }

        if(bits & AS_EVENT_WAKE_WORD_RUNNING){
                // For wake word detection, directly feed audio data to audio processor without encoding, to reduce latency
                std::vector<int16_t> data;
                int samples = wake_word_ ? wake_word_->GetFeedSize() : 0;
                if(samples <= 0){
                    samples = audio_processor_->GetFeedSize();
                }
                if(samples > 0){
                    if(ReadAudioData(data, 16000, samples)){
                        if(wake_word_){
                            wake_word_->Feed(data);
                        }
                        continue;
                    }
                }
        }

        if(bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING){
            std::vector<int16_t> data;
            int samples = audio_processor_->GetFeedSize();
            if(samples > 0){
                if(ReadAudioData(data, 16000, samples)){
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
            }
            ESP_LOGI(TAG, "Should not be here, bits: %lx", bits);
            break;
        
        }
    }
    
   
}



void AudioService::AudioOutputTask(){
    while(true){
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]{
            return !audio_playback_queue_.empty() || service_stopped_;
        });
        if(service_stopped_){
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        codec_->OutputData(task->pcm);

        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;
    }
    ESP_LOGI(TAG, "Audio output task stopped");
}


bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rates,int samples){
    if(!codec_->input_enabled()){
    esp_timer_stop(audio_power_timer_);
    esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
    codec_->EnableInput(true);
    }

    data.resize(samples* codec_->input_channels());
    if(!codec_->InputData(data)){
        return false;
    }

    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;
    return true;
}

void AudioService::OpusCodecTask(){
    while(true){
            std::unique_lock<std::mutex> lock(audio_queue_mutex_);
            audio_queue_cv_.wait(lock, [this]{return (!audio_encode_queue_.empty()&&audio_send_queue_.size()<MAX_SEND_PACKETS_IN_QUEUE)
                                                || (!audio_decode_queue_.empty()&&audio_playback_queue_.size()<MAX_PLAYBACK_TASKS_IN_QUEUE)
                                                || service_stopped_;});
        

        if(service_stopped_){
            break;
        }

        /* Decode the audio from decode queue */
        //解码队列不为空且播放队列大小未达上限时，从解码队列取出数据进行解码
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE){
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;
            
            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
        

            if(opus_decoder_!=nullptr){
                task->pcm.resize(decoder_frame_size*codec_->output_channels());
                
                esp_audio_dec_in_raw_t in = {
                    .buffer = (uint8_t*)packet->payload.data(),
                    .len = (uint32_t)packet->payload.size(),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };

                esp_audio_dec_out_frame_t out = {
                    .buffer = (uint8_t*)task->pcm.data(),
                    .len = (uint32_t)task->pcm.size() * sizeof(int16_t),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &in, &out, &dec_info);
                decoder_lock.unlock();
                if(ret==ESP_AUDIO_ERR_OK){
                    task->pcm.resize(out.decoded_size/ sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    lock.lock();
                    audio_playback_queue_.push_back(std::move(task));
                    audio_queue_cv_.notify_all();
                    debug_statistics_.decode_count++;
                }else{
                    ESP_LOGI(TAG, "Failed to decode audio frame, error code: %d", ret);
                    lock.lock();
                }
            }else{
                ESP_LOGI(TAG, "Opus decoder is not initialized");
                lock.lock();
            }

        }

        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);                   
                    if(task->type == kAudioTaskTypeEncodeToSendQueue){
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_send_queue_.push_back(std::move(packet));
                        if(callbacks_.on_send_queue_available){
                            callbacks_.on_send_queue_available();
                        }
                    }if(task->type == kAudioTaskTypeEncodeToTestingQueue){
                        // For testing purpose, directly push the encoded packet to playback queue to verify the whole encoding-decoding process
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                }else{
                    ESP_LOGI(TAG, "Failed to encode audio frame, error code: %d", ret);
                }

            }else{
                ESP_LOGI(TAG, "Failed to encode audio: encoder not configured or invalid frame size (got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
        }
        lock.lock();
        }


    }
    ESP_LOGI(TAG, "Opus codec task stopped");

}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration)
{
    if(decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration){
        return;
    }
  
    std::unique_lock<std::mutex> decoder_lock(audio_queue_mutex_);
    if(opus_decoder_!=nullptr){
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if(opus_decoder_==nullptr){
        ESP_LOGI(TAG,"Fail to create audio decoder,error code:%d",ret);
    }
    else{
        decoder_sample_rate_ = sample_rate;
        decoder_duration_ms_ = frame_duration;
        decoder_frame_size = decoder_sample_rate_ / 1000 * decoder_duration_ms_;
        ESP_LOGI(TAG, "Audio decoder sample rate set to %d, frame duration set to %d ms", decoder_sample_rate_, decoder_duration_ms_);
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    if(decoder_sample_rate_ != codec->output_sample_rate()){
        ESP_LOGI(TAG, "Audio decoder sample rate (%d) is different from codec output sample rate (%d), there might be audio quality issue", decoder_sample_rate_, codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGI(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }



}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm){
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);

    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGI(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}




void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::SetModelsList(srmodel_list_t* models_list){
    models_list_ = models_list;
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AFEWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    // 初始化音频前处理（AFE）：AEC / NS / VAD
    if (audio_processor_ && !audio_processor_initialized_ && codec_ != nullptr) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = true;
    }

    // 初始化唤醒词引擎
    if(wake_word_ && !wake_word_initialized_ && codec_ != nullptr){
        if (!wake_word_->Initialize(codec_, models_list_)) {
            ESP_LOGW(TAG, "Failed to initialize wake word engine");
            wake_word_.reset();
        } else {
            wake_word_initialized_ = true;
        }
    }

    if(wake_word_){
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word){
            if(callbacks_.on_wake_word_detected){
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }

}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (wake_word_ == nullptr) {
        if (enable) {
            ESP_LOGW(TAG, "Wake word is not initialized");
        }
        if (!enable) {
            xEventGroupClearBits(audio_event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        }
        return;
    }
    if (enable) {
        wake_word_->Start();
        xEventGroupSetBits(audio_event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        ESP_LOGI(TAG, "Wake word detection enabled");
    } else {
        xEventGroupClearBits(audio_event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        wake_word_->Stop();
        ESP_LOGI(TAG, "Wake word detection disabled");
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    if (audio_processor_ == nullptr) {
        return;
    }
    if (enable) {
        if (!audio_processor_initialized_ && codec_ != nullptr) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }
        audio_processor_->Start();
        xEventGroupSetBits(audio_event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        ESP_LOGI(TAG, "Voice processing enabled");
    } else {
        xEventGroupClearBits(audio_event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        audio_processor_->Stop();
        ESP_LOGI(TAG, "Voice processing disabled");
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    if (enable) {
        xEventGroupSetBits(audio_event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(audio_event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    if (audio_processor_) {
        audio_processor_->EnableDeviceAec(enable);
    }
}

bool AudioService::IsWakeWordRunning() const {
    return xEventGroupGetBits(audio_event_group_) & AS_EVENT_WAKE_WORD_RUNNING;
}

bool AudioService::IsAudioProcessorRunning() const {
    return xEventGroupGetBits(audio_event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING;
}

bool AudioService::IsAudioTestingRunning() const {
    return xEventGroupGetBits(audio_event_group_) & AS_EVENT_AUDIO_TESTING_RUNNING;
}
 

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    if (packet == nullptr) {
        return false;
    }
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (!wait) {
            return false;
        }
        audio_queue_cv_.wait(lock, [this]() {
            return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE || service_stopped_;
        });
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}





