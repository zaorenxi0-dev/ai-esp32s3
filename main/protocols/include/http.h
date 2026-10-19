#pragma once
#include <string>
#include <map>

class Http {
public:

    virtual ~Http();

    virtual void SetTimeout(int timeout_ms) = 0;

    virtual void SetHeader(const std::string& key, const std::string& value) = 0;

    virtual void SetContent(std::string&& content) = 0;

    virtual void SetKeepAlive(bool keep_alive) = 0;

    // 打开 HTTP 连接并发送请求
    virtual bool Open(const std::string& method, const std::string& url) = 0;

    // 关闭 HTTP 连接
    virtual void Close() = 0;    

    virtual int Read(char* buffer, size_t buffer_size) = 0;

    virtual int Write(const char* buffer, size_t buffer_size) = 0;

    // 获取 HTTP 响应状态码
    virtual int GetStatusCode() = 0;
    
    // 获取指定 key 的 HTTP 响应头
    virtual std::string GetResponseHeader(const std::string& key) const = 0;

    // 获取 HTTP 响应体长度
    virtual size_t GetBodyLength() = 0;

    // 获取 HTTP 响应体
    virtual std::string ReadAll() = 0;

    // 获取最后一次错误码
    virtual int GetLastError() = 0;    
};