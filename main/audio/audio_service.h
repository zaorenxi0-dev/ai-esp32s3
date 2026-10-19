#pragma once

#include <esp_err.h>
#include <esp_log.h>

#include <memory>
#include <mutex>
#include <deque>
#include <condition_variable>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include "esp_audio_enc.h"
#include "esp_audio_dec.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"

#include "protocols/protocol.h"
#include "audio_codec.h"
#include "audio_processor.h"
#include "wake_word.h"
#include "boards/common/board.h"
#include "audio/wake_words/afe_wake_word.h"
#include "audio/wake_words/custom_wake_word.h"

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                   \
    ((duration_ms) == 5 ? ESP_OPUS_ENC_FRAME_DURATION_5_MS :      \
     (duration_ms) == 10 ? ESP_OPUS_ENC_FRAME_DURATION_10_MS :    \
     (duration_ms) == 20 ? ESP_OPUS_ENC_FRAME_DURATION_20_MS :    \
     (duration_ms) == 40 ? ESP_OPUS_ENC_FRAME_DURATION_40_MS :    \
     (duration_ms) == 60 ? ESP_OPUS_ENC_FRAME_DURATION_60_MS :    \
     (duration_ms) == 80 ? ESP_OPUS_ENC_FRAME_DURATION_80_MS :    \
     (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS :  \
     (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS : -1)

#define AS_OPUS_ENC_CONFIG()  {\
        .sample_rate  =ESP_AUDIO_SAMPLE_RATE_16K,                                                                 \
        .channel      =ESP_AUDIO_MONO,\
        .bits_per_sample    = ESP_AUDIO_BIT16,                                                                    \
        .bitrate            = ESP_OPUS_BITRATE_AUTO,                                                              \
        .frame_duration     = (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS),  \
        .application_mode   = ESP_OPUS_ENC_APPLICATION_AUDIO,                                                     \
        .complexity         = 0,                                                                                  \
        .enable_fec         = false,                                                                              \
        .enable_dtx         = true,                                                                               \
        .enable_vbr         = true,   \
}

struct AudioServiceCallbacks
{
   std::function<void(bool)> on_vad_change;
   std::function<void(void)> on_send_queue_available; 
   std::function<void(const std::string&)> on_wake_word_detected;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};


class AudioService{
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void SetCallbacks(AudioServiceCallbacks& callbacks);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rates,int samples);
    void SetModelsList(srmodel_list_t* models_list);
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();

    // ---- 语音链路控制（事件组通知 + 任务协调） ---------------------------
    void EnableWakeWordDetection(bool enable);
    void EnableVoiceProcessing(bool enable);
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);

    bool IsWakeWordRunning() const;
    bool IsAudioProcessorRunning() const;
    bool IsAudioTestingRunning() const;
private:
    AudioCodec* codec_;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;

    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;


    EventGroupHandle_t audio_event_group_;

    //encode/decode state
    int decoder_sample_rate_=0;
    int decoder_duration_ms_=OPUS_FRAME_DURATION_MS;
    int decoder_frame_size=0;
    int encoder_sample_rate_ = 16000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    srmodel_list_t* models_list_ = nullptr;

    bool voice_detected_ = false;
    bool service_stopped_ = true;
    bool audio_input_need_warmup_ = false;
    bool audio_processor_initialized_ = false;
    bool wake_word_initialized_ = false;

    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::mutex decoder_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;    
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;//待发送队列
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;//编码任务
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;//播放任务

    std::deque<uint32_t> timestamp_queue_;

    DebugStatistics debug_statistics_;


    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();

    void CheckAndUpdateAudioPowerState();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);    
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
};



