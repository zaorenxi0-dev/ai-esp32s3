#pragma once


#include <memory>
#include <map>
#include <string>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <sstream>

#include "http.h"
#include "tcp.h"
#include "esp_tcp.h"
#include "network_interface.h"

#include <freertos/event_groups.h>

#define EC801E_HTTP_EVENT_HEADERS_RECEIVED (1 << 0)
#define EC801E_HTTP_EVENT_BODY_RECEIVED (1 << 1)
#define EC801E_HTTP_EVENT_ERROR (1 << 2)
#define EC801E_HTTP_EVENT_COMPLETE (1 << 3)

class HttpClient : public Http {
public:
    HttpClient(NetworkInterface* network, int connect_id = 0);
    ~HttpClient();

    void SetTimeout(int timeout_ms) override;
    void SetHeader(const std::string& key, const std::string& value) override;
    void SetContent(std::string&& content) override;
    void SetKeepAlive(bool keep_alive) override;
    bool Open(const std::string& method, const std::string& url) override;
    void Close() override;
    int Read(char* buffer, size_t buffer_size) override;
    int Write(const char* buffer, size_t buffer_size) override;

    int GetStatusCode() override;
    std::string GetResponseHeader(const std::string& key) const override;
    int GetLastError() override;
    std::string ReadAll() override;
    size_t GetBodyLength() override;
    std::string BuildHttpRequest();



private:
    struct DataChunk{
        std::string data;
        size_t offset = 0;  // 当前读取偏移
        // 使用移动构造函数避免拷贝
        DataChunk(std::string&& d) : data(std::move(d)), offset(0) {}
        DataChunk(const std::string& d) : data(d), offset(0) {}

        size_t available() const{
            return data.size() - offset;
        }
        size_t read(char* buffer, size_t buffer_size){
            size_t read_size = std::min(buffer_size, available());
            if(read_size > 0){
                memcpy(buffer, data.data() + offset, read_size);
                offset += read_size;
            }

            return read_size;
        }

        bool empty() const {
            return offset >= data.size();
        }
    };

    struct HeaderEntry {
        std::string original_key;
        std::string value;
        std::string url_;
        std::string method_;


        HeaderEntry() = default;
        HeaderEntry(const std::string& key, const std::string& value) : original_key(key), value(value) {}
    };

    std::string url_;
    std::string method_;
    std::string protocol_;
    std::map<std::string, HeaderEntry> headers_;  // key为小写，用于快速查找
    std::optional<std::string> content_ = std::nullopt;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, HeaderEntry> response_headers_;  

    std::mutex read_mutex_;
    std::condition_variable write_cv_;
    std::deque<DataChunk> body_chunks_;
    const size_t MAX_BODY_CHUNKS_SIZE = 8192;

    size_t content_length_ = 0;
    size_t total_body_received_ = 0;  // 总共接收的响应体字节数
    size_t chunk_size_ = 0;
    size_t chunk_received_ = 0;
    size_t body_offset_ = 0;

    int timeout_ms_ = 30000;
    int status_code_ = -1;
    int port_ = 80;
    std::string rx_buffer_;
    std::string path_;
    std::string host_;
    int last_error_ = 0;

    bool eof_ = false;
    bool response_chunked_ = false;
    bool request_chunked_ = false;
    bool connected_ = false;
    bool server_keep_alive_ = false;  // 新增：服务器是否支持 Keep-Alive
    bool connection_error_ = false;  // 新增：标记连接是否异常断开
    bool keep_alive_ = false;  // 新增：是否启用 Keep-Alive（默认不启用）
    bool headers_received_ = false;
        // HTTP 协议解析状态
    enum class ParseState {
        STATUS_LINE,
        HEADERS,
        BODY,
        CHUNK_SIZE,
        CHUNK_DATA,
        CHUNK_TRAILER,
        COMPLETE
    };
    ParseState parse_state_ = ParseState::STATUS_LINE;

    std::unique_ptr<Tcp> tcp_;
    NetworkInterface* network_;
    EventGroupHandle_t event_group_handle_;


    void OnTcpDisconnected(); 
    bool ParseHeaderLine(const std::string& line);
    std::string GetNextLine(std::string& buffer);
    void OnTcpData(const std::string& data);
    bool ParseUrl(const std::string& url);
    void ResetRequestState();
    bool IsConnectionReusable(const std::string& host, int port);
    bool IsDataComplete() const;
    bool HasCompleteLine(const std::string& buffer);
    void ProcessReceivedData();
    void ParseRegularBody(const std::string& data);
    void ParseChunkedBody(const std::string& data);
    bool ParseStatusLine(const std::string& line);
    void AddBodyData(const std::string& data);
    void AddBodyData(std::string&& data);
    size_t ParseChunkSize(const std::string& chunk_size_line);
    void SetError();
};




