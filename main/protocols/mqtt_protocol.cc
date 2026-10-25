#include "mqtt_protocol.h"

#define TAG "MqttProtocol"

#include "setting.h"



MqttProtocol::MqttProtocol()
{
    event_group_ = xEventGroupCreate();

    esp_timer_create_args_t reconnect_timer_args = {
        .callback=[](void* arg){
            auto* protocol = static_cast<MqttProtocol*>(arg);
            auto& app = Application::GetInstance();
            if(app.GetDeviceState()==kDeviceStateIdle){
                auto alive = protocol->alive_;
                app.Schedule([protocol,alive](){
                    if((*alive))
                    {
                        protocol->StartMqttClient(false);
                    }
                });
            }
        },
        .arg=static_cast<void*>(this),
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);

}

MqttProtocol::~MqttProtocol()
{
}

bool MqttProtocol::Start()
{
    return StartMqttClient(false);
}

bool MqttProtocol::StartMqttClient(bool report_error)
{
    if(mqtt_!=nullptr)
    {
        ESP_LOGI(TAG, "Mqtt client already started");
        mqtt_.reset();   
    }


    Setting settings("mqtt",false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive_interval",240);
    publish_topic_ = settings.GetString("publish_topic");

    if(endpoint.empty())
    {
        ESP_LOGI(TAG, "Endpoint is empty");
        if(report_error)
        {
            SetError("Endpoint is empty");
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepaliveInterval(keepalive_interval);

    mqtt_->OnConnected([this](){
        if(on_connected_!=nullptr)
        {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnDisconnected([this](){
        if(on_disconnected_!=nullptr)
        {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds", MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS*1000);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload){
        cJSON* root = cJSON_Parse(payload.c_str());
        if(root==nullptr)
        {
            ESP_LOGI(TAG, "Failed to parse JSON payload");
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGI(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if(strcmp(type->valuestring,"hello")==0)
        {
            ParseServeHello(root);
        }else if(strcmp(type->valuestring,"goodbye")==0)
        {
           auto session_id = cJSON_GetObjectItem(root, "session_id");
           // 原来这里写的是 strcmp(session_id->valuestring, session_id->valuestring)，自己和自己比，
           // 恒等于 0（永远成立）；且 JSON 里 "session_id":null 时 valuestring 是空指针会崩。
           const char* sid = (cJSON_IsString(session_id) && session_id->valuestring != nullptr)
                                 ? session_id->valuestring : nullptr;
           ESP_LOGI(TAG, "Received goodbye message, session_id: %s (local=%s)",
                    sid ? sid : "null", session_id_.c_str());
           if (sid == nullptr || session_id_ == sid) {
               auto alive = alive_;
               Application::GetInstance().Schedule([this,alive](){
                   if((*alive))
                   {
                       CloseAudioChannel();
                   }
               });
           }
        }else if(on_incoming_json_!=nullptr)
        {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });
   ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint.c_str());
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(":");
    if(pos!=std::string::npos)
    {
        broker_address = endpoint.substr(0,pos);
        broker_port = std::stoi(endpoint.substr(pos+1));
    }else
    {
        broker_address = endpoint;
    }
    if(!mqtt_->Connect(broker_address, broker_port, client_id, username, password))
    {
        ESP_LOGI(TAG, "Failed to connect to broker");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    ESP_LOGI(TAG, "Connected to endpoint");
    return true;

}

bool MqttProtocol::SendText(const std::string& text)
{
    if(publish_topic_.empty())
    {
        ESP_LOGI(TAG, "Publish topic is empty");
        return false;
    }
    if(!mqtt_->Publish(publish_topic_, text))
    {
        ESP_LOGI(TAG, "Failed to publish message");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool MqttProtocol::SendMcpMessage(const std::string& message)
{
    // MQTT 通道上，MCP 的 JSON-RPC 必须包在 {"type":"mcp","payload":{...}} 里，
    // 网关靠 type 字段分发；裸 JSON-RPC 会被直接丢弃，握手永远完不成。
    std::string text = "{\"type\":\"mcp\",\"payload\":";
    text += message;
    text += "}";
    ESP_LOGI(TAG, "Send MCP: %s", text.c_str());
    return SendText(text);
}

bool MqttProtocol::SendAbort()
{
    // 会话没开就别发了，服务端收到也没意义，只在日志里留一条
    if (!IsAudioChannelOpened()) {
        ESP_LOGW(TAG, "SendAbort ignored: audio channel not open");
        return false;
    }
    ESP_LOGI(TAG, "Send abort, session_id=%s", session_id_.c_str());
    // 报文由基类按标准协议拼（带 session_id + reason），这里只补日志和通道检查
    return Protocol::SendAbort();
}

void MqttProtocol::CloseAudioChannel()
{
    bool was_open = false;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        // 幂等：超时看门狗、send 失败、服务端 goodbye 三条路都可能同时走到这里，
        // 重复关会重复发 goodbye，还会重复回调把状态来回切。
        // udp_ 非空才代表「通道确实开着」，用它当唯一凭据。
        was_open = (udp_ != nullptr);
        udp_.reset();
    }
    if (!was_open) {
        ESP_LOGI(TAG, "CloseAudioChannel: channel already closed, ignored");
        return;
    }
    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    SendText(message);
 
    if(on_audio_channel_closed_!=nullptr)
    {
        on_audio_channel_closed_();
    }
}

void MqttProtocol::ParseServeHello(cJSON* root)
{
    // 先原样打出服务端 hello，任何字段不匹配都能一眼看出来
    char* raw = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Server hello: %s", raw ? raw : "(null)");
    if (raw) {
        cJSON_free(raw);
    }

    auto tranport = cJSON_GetObjectItem(root, "transport");
    if (tranport == nullptr || !cJSON_IsString(tranport) || strcmp(tranport->valuestring, "udp") != 0) {
        ESP_LOGW(TAG, "Transport is invalid: %s",
                 (tranport && cJSON_IsString(tranport)) ? tranport->valuestring : "(missing)");
        SetError("Server does not support UDP transport");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }
    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGW(TAG, "UDP is not specified in server hello");
        SetError("Server hello has no udp section");
        return;
    }
    auto udp_server_item = cJSON_GetObjectItem(udp, "server");
    auto udp_port_item = cJSON_GetObjectItem(udp, "port");
    auto key_item = cJSON_GetObjectItem(udp, "key");
    auto nonce_item = cJSON_GetObjectItem(udp, "nonce");
    if (!cJSON_IsString(udp_server_item) || !cJSON_IsNumber(udp_port_item) ||
        !cJSON_IsString(key_item) || !cJSON_IsString(nonce_item)) {
        ESP_LOGW(TAG, "UDP section is incomplete (server/port/key/nonce)");
        SetError("Server hello udp section is incomplete");
        return;
    }
    udp_server_ = udp_server_item->valuestring;
    udp_port_ = udp_port_item->valueint;
    auto key = key_item->valuestring;
    auto nonce = nonce_item->valuestring;

    aes_nonce_ = DecodeHexString(nonce);
    mbedtls_aes_init(&aes_ctx_);

    mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)DecodeHexString(key).c_str(), 128);
    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}
static inline uint8_t CharToHex(char c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0; 
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string){
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::OpenAudioChannel(){
    if(mqtt_ == nullptr || !mqtt_->IsConnected())
    {
        ESP_LOGI(TAG, "Not connected");
        if(!StartMqttClient(true)){
            ESP_LOGI(TAG, "Failed to start MQTT client");
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    // 关键：通道打开时必须把「最后收包时间」打到当前时刻。
    // 这个变量默认构造是 steady_clock 的纪元（很久以前），如果不在开通道时刷新，
    // IsTimeout() 会立刻返回 true，IsAudioChannelOpened() 从第一秒起就是 false。
    last_incoming_time_ = std::chrono::steady_clock::now();
    xEventGroupClearBits(event_group_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if(!SendText(message))
    {
        ESP_LOGI(TAG, "Failed to send hello message");
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(event_group_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGI(TAG, "Server hello timeout");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    udp_.reset();   // 上一轮通道的 UDP 先扔掉，避免旧对象残留导致 IsAudioChannelOpened 误判
    udp_ = network->CreateUdp();
    udp_->OnMessage([this](const std::string& data){
        if (data.size() < sizeof(aes_nonce_)) {
            ESP_LOGI(TAG, "Invalid audio packet size: %u", data.size());
            return;
        }
        if (data[0] != 0x01) {
            ESP_LOGI(TAG, "Invalid audio packet type: %x", data[0]);
            return;
        }
        uint32_t timestamp = ntohl(*(uint32_t*)&data[8]);
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);
        if(sequence<remote_sequence_){
             ESP_LOGI(TAG, "Received audio packet with old sequence: %lu, expected: %lu", sequence, remote_sequence_);
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            ESP_LOGI(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu", sequence, remote_sequence_ + 1);
        }

        size_t decrypted_size = data.size() - aes_nonce_.size();
        uint8_t stream_block[16]={0};
        size_t nc_off=0;

        uint8_t nonce[16] = {0};
        memcpy(nonce, data.data(), std::min(sizeof(nonce), aes_nonce_.size()));
        auto encrypted = (const uint8_t*)data.data() + aes_nonce_.size();
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->timestamp = timestamp;
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->payload.resize(decrypted_size);
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size,&nc_off,nonce,stream_block,encrypted,(uint8_t*)packet->payload.data());
        if(ret!=0){
            ESP_LOGI(TAG, "Failed to decrypt audio packet");
            return;
        }
        if(on_incoming_audio_!=nullptr){
            on_incoming_audio_(std::move(packet));
        }
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
        // 诊断：服务端下发的音频包计数，确认下行通道是否打通
        static int rx_count = 0;
        rx_count++;
        if (rx_count <= 3 || rx_count % 100 == 0) {
            ESP_LOGI(TAG, "[UDP RX] count=%d seq=%lu size=%u", rx_count, sequence, (unsigned)decrypted_size);
        }
    });
    udp_->Connect(udp_server_, udp_port_);
    if(on_audio_channel_opened_!=nullptr){
        on_audio_channel_opened_();
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet){

    std::lock_guard<std::mutex> lock(channel_mutex_);
    if(udp_==nullptr){
        ESP_LOGI(TAG, "UDP not connected");
        return false;
    }
    // 通道已超时（服务端早把会话结束了）还继续发，只会白烧 CPU 和流量，
    // 日志还显示「一直在发」，把真正的问题掩盖掉。这里直接拒绝，交给上层收尾。
    if(IsTimeout()){
        ESP_LOGW(TAG, "Audio channel timed out, dropping packet");
        return false;
    }

    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(packet->payload.size());
    *(uint32_t*)&nonce[8] = htonl(packet->timestamp);
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(packet->payload.size()+aes_nonce_.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, packet->payload.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)packet->payload.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        ESP_LOGI(TAG, "Failed to encrypt audio data");
        return false;
    }
    // 诊断：上行音频包计数。设备侧认为是「发成功」的，但如果收不到计数，
    // 说明 UDP 根本没出去（安全组没放行 8884/udp 是最常见原因）
    static int tx_count = 0;
    static int tx_fail = 0;
    int sent = udp_->Send(encrypted);
    if (sent <= 0) {
        tx_fail++;
        if (tx_fail <= 5) {
            ESP_LOGW(TAG, "[UDP TX] send failed ret=%d fails=%d", sent, tx_fail);
        }
        return false;
    }
    tx_count++;
    if (tx_count <= 3 || tx_count % 100 == 0) {
        ESP_LOGI(TAG, "[UDP TX] count=%d seq=%lu bytes=%u", tx_count, local_sequence_, (unsigned)encrypted.size());
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage()
{
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}


bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
