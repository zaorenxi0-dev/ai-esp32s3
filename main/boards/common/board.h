#pragma once
#include "audio_codec.h"
#include "display.h"
#include "network_interface.h"

enum class NetworkEvent {
    Scanning,              // Network is scanning (WiFi scanning, etc.)
    Connecting,            // Network is connecting (data: SSID/network name)
    Connected,             // Network connected successfully (data: SSID/network name)
    Disconnected,          // Network disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode
    WifiConfigModeExit,    // Exited WiFi configuration mode
    // Cellular modem specific events
    ModemDetecting,        // Detecting modem (baud rate, module type)
    ModemErrorNoSim,       // No SIM card detected
    ModemErrorRegDenied,   // Network registration denied
    ModemErrorInitFailed,  // Modem initialization failed
    ModemErrorTimeout      // Operation timeout
};

enum class PowerSaveLevel {
    LOW_POWER,    // Maximum power saving (lowest power consumption)
    BALANCED,     // Medium power saving (balanced)
    PERFORMANCE,  // No power saving (maximum power consumption / full performance)
};
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;
void* create_board();
class Board{

private:

    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();
    // 软件生成的设备唯一标识
    std::string uuid_;
public:

    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }
    virtual ~Board();
    virtual Display* GetDisplay();
    virtual AudioCodec* GetAudioCodec()=0;
    virtual std::string GetUuid() { return uuid_; }
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
    virtual std::string GetSystemInfoJson();
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
};


// 定义宏：DECLARE_BOARD 是宏名，BOARD_CLASS_NAME 是宏的参数（你传的类名）
#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}// 核心：创建你指定的类的对象，返回指针


