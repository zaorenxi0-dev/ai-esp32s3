#include "audio/codes/no_audio_code.h"

#include <esp_log.h>
#include <cstring>
#include <cmath>

#define TAG "NoAudioCodec"

NoAudioCode::~NoAudioCode() {

    if (i2s_tx_handle_) {
        ESP_ERROR_CHECK(i2s_del_channel(i2s_tx_handle_));
    }
    if (i2s_rx_handle_) {
        ESP_ERROR_CHECK(i2s_del_channel(i2s_rx_handle_));
    }
}


NoAudioCodeDuolex::NoAudioCodeDuolex(int input_sample_rate,int output_sample_rate,gpio_num_t bclk,gpio_num_t ws,gpio_num_t din,gpio_num_t dout){
    





}

NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din){
     duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t chan_cfg = {
        .id = (i2s_port_t)0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle_, &std_cfg));

    // Create a new channel for MIC
    chan_cfg.id = (i2s_port_t)1;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &i2s_tx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Simplex channels created");
}

NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck,gpio_num_t mic_ws, gpio_num_t mic_din,i2s_std_slot_mask_t mic_slot_mask){
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    
    i2s_chan_config_t tx_chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,//dma道路数，决定了可以同时使用多少个dma buffer
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,//每个dma buffer中包含的frame数，决定了每次回调传输的数据量，frame数越多，回调越不频繁，但每次传输的数据量也越大
        .auto_clear_after_cb = true,//是否在发送回调后自动清除dma buffer，设置为true后，i2s会在发送回调后自动清除dma buffer中的数据，这样用户就可以直接在回调中给dma buffer赋值，而不需要担心数据在退出回调后被清除
        .auto_clear_before_cb = false, //是否在发送回调前自动清除dma buffer，设置为true后，i2s会在发送回调前自动清除dma buffer中的数据，这样用户就可以在回调中访问刚刚发送完成的数据
        .allow_pd = true,//是否允许电源关闭，设置为true后，驱动会在进入/退出睡眠模式前备份/恢复i2s寄存器，这样系统就可以关闭i2s的电源域，这可以节省电量，但代价是会消耗更多的内存
        .intr_priority = 1//中断优先级，范围[0, 7]，如果设置为0，驱动会尝试分配一个相对较低优先级的中断（1,2,3）
    };
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &i2s_tx_handle_, nullptr));

    i2s_std_config_t std_cfg={
            .clk_cfg ={
                .sample_rate_hz =(uint32_t) output_sample_rate,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256, 
            },
            .slot_cfg={
                .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_MONO,
                .slot_mask = spk_slot_mask,//选择要启用的时隙（声道）
                .ws_width = I2S_DATA_BIT_WIDTH_32BIT ,
                .ws_pol = 0,
                .bit_shift = 1,

            },
            .gpio_cfg={
                .mclk = GPIO_NUM_NC,
                .bclk = spk_bclk,
                .ws = spk_ws,
                .dout = spk_dout,
                .din = GPIO_NUM_NC,
                .invert_flags = {
                    .mclk_inv = 0,
                    .bclk_inv = 0,
                    .ws_inv = 0,
                }
            },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle_, &std_cfg));

    tx_chan_cfg.id = I2S_NUM_1;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, nullptr, &i2s_rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t) input_sample_rate;
    std_cfg.slot_cfg.slot_mask = mic_slot_mask;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.dout = GPIO_NUM_NC;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "NoAudioCodecSimplexPdm");

}
int NoAudioCode::Read(int16_t* dest, int samples){
    // 默认实现：从 I2S RX 通道读取 32bit 数据并转换为 16bit 输出。
    // 具体板级子类（如 NoAudioCodecSimplex）可覆写此方法。
    if (i2s_rx_handle_ == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    size_t bytes_read = 0;
    std::vector<int32_t> bit32_buffer(samples);

    if (i2s_channel_read(i2s_rx_handle_, bit32_buffer.data(), samples * sizeof(int32_t),
                         &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGI(TAG, "Failed to read data from I2S channel");
        return 0;
    }
    int read_samples = bytes_read / sizeof(int32_t);
    for (int i = 0; i < read_samples; i++) {
        int32_t temp = bit32_buffer[i] >> 12;
        if (temp > INT16_MAX) {
            temp = INT16_MAX;
        } else if (temp < INT16_MIN) {
            temp = INT16_MIN;
        }
        dest[i] = int16_t(temp);
    }
    return read_samples;
}

int NoAudioCode::Write(const int16_t* data,int samples){
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    std::vector<int32_t>buffer(samples);

    int32_t volume_factor = pow(double(output_volume_) / 100, 2)*65535; // Calculate volume factor based on the output volume percentage

    for(int i = 0; i < samples; i++){
        int64_t temp= int64_t(data[i]) * volume_factor; // Apply volume factor to the sample
        if(temp>INT32_MAX) 
        temp = INT32_MAX; // Clip to int16_t range
        else if
        (temp<INT32_MIN) 
        temp = INT32_MIN;
        buffer[i] = int32_t(temp); // Store the processed sample in the buffer
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(i2s_tx_handle_, buffer.data(), samples * sizeof(int32_t), &bytes_written, portMAX_DELAY));
    return bytes_written/ sizeof(int32_t);
}


int NoAudioCodecSimplex::Read(int16_t* dest, int samples){
    size_t bytes_read;

    std::vector<int32_t> bit32_buffer(samples);

    if(i2s_channel_read(i2s_rx_handle_, bit32_buffer.data(), samples * sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK){
        ESP_LOGI(TAG, "Failed to read data from I2S channel");
        return 0;
    }
    samples = bytes_read/ sizeof(int32_t);

    for(int i = 0; i < samples; i++){
        int32_t temp = bit32_buffer[i] >> 12; //为什么右移12位？因为在i2s_std_slot_config_t中，数据位宽设置为32位，但实际有效数据位宽可能只有20位（例如24位数据被放在32位的slot中），所以需要右移12位来获取有效的16位音频数据。这个右移操作相当于将32位数据中的高20位丢弃，只保留低12位，这样就得到了原始的16位音频样本。之后再进行音量调整和范围限制，最终存储到dest数组中。
        if(temp>INT16_MAX) 
        temp = INT16_MAX; // Clip to int16_t range
        else if
        (temp<INT16_MIN) 
        temp = INT16_MIN;
        dest[i] = int16_t(temp); // Store the processed sample in the destination buffer
    }
    return samples;
}




