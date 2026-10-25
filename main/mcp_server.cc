#include "mcp_server.h"
#include "easymatrix.h"
#include "ble.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <esp_app_desc.h>
#include "sdkconfig.h"

#define TAG "MCPServer"

namespace {

// 解析 "RRGGBB" 或 "#RRGGBB"。
// 原实现直接 strtoul(color, nullptr, 16)：非法字符会被静默当成 0（变成纯黑），
// 8 位串还会被当成带 alpha 的整数截断，云端传错格式时完全无感知。
bool ParseHexColor(const std::string& text, uint8_t* r, uint8_t* g, uint8_t* b) {
    size_t i = (!text.empty() && text[0] == '#') ? 1 : 0;
    if (text.size() - i != 6) {
        return false;
    }
    uint32_t v = 0;
    for (; i < text.size(); ++i) {
        char c = text[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *r = (v >> 16) & 0xFF;
    *g = (v >> 8) & 0xFF;
    *b = v & 0xFF;
    return true;
}

}  // namespace
MCPServer::MCPServer() {
    // 初始化服务器
}

MCPServer::~MCPServer() {
    // 清理服务器资源
}




void MCPServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMCPMessage(payload);
}

void MCPServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMCPMessage(payload);
}

void MCPServer::AddTool(McpTool* tool){
    if(std::find_if(tools_.begin(),tools_.end(),[tool](const McpTool* t){return t->name()==tool->name();})!=tools_.end()){
        ESP_LOGI(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }
    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);

}

void MCPServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback){
    AddTool(new McpTool(name,description,properties,callback));
}

void MCPServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void MCPServer::AddCommonTools() {

    auto& board = Board::GetInstance();

    AddTool("self.get_device", 
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
        return board.GetDeviceStatusJson();
        });


    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            if (codec != nullptr) {
                codec->SetOutputVolume(properties["volume"].value<int>());
            }
            return std::string("ok");
        });

    // 像素绘画 / WS2812 LED 矩阵工具入口
    AddLedMatrixTools();

    // BLE 蓝牙像素绘画控制通道入口
    AddBleTools();
}

void MCPServer::AddUserOnlyTools() {
}

void MCPServer::AddBleTools() {
#if CONFIG_BT_ENABLED
    AddTool("self.pixel_art.enable_ble",
        "开启 BLE 蓝牙像素绘画模式。当用户希望通过蓝牙 / 微信小程序控制 LED 矩阵像素画时调用。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            (void)properties;
            static BLE ble;
            static std::once_flag once;
            static esp_err_t init_result = ESP_OK;
            // nimble_port_init() 不可重入，用 call_once 保证只初始化一次；
            // 原实现用普通 static bool，两个 MCP 请求并发时都会进来。
            std::call_once(once, [&]() { init_result = ble.init(); });
            if (init_result != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize BLE, err: %d", init_result);
                return std::string("failed to init BLE");
            }
            return std::string("ble pixel-art enabled");
        });
#else
    ESP_LOGI(TAG, "BLE disabled (CONFIG_BT_ENABLED=n), pixel-art BLE tool not registered");
#endif
}

void MCPServer::AddLedMatrixTools() {
    EasyMatrix* matrix = GetEasyMatrix();
    if (matrix == nullptr || !matrix->initialized()) {
        ESP_LOGW(TAG, "LED matrix is not available, pixel drawing tools disabled");
        return;
    }

    AddTool("self.led_matrix.set_pixel",
        "点亮 WS2812 LED 矩阵上的一个像素（像素绘画）。坐标原点在左上角，x 向右增大、y 向下增大；"
        "颜色为 6 位十六进制 RRGGBB。",
        PropertyList({
            Property("x", kPropertyTypeInteger, 0, matrix->width() - 1),
            Property("y", kPropertyTypeInteger, 0, matrix->height() - 1),
            Property("color", kPropertyTypeString, std::string("FF0000"))
        }),
        [matrix](const PropertyList& properties) -> ReturnValue {
            std::string color = properties["color"].value<std::string>();
            uint8_t r = 0, g = 0, b = 0;
            if (!ParseHexColor(color, &r, &g, &b)) {
                return std::string("invalid color: expect 6-digit RRGGBB");
            }
            matrix->SetPixel(properties["x"].value<int>(), properties["y"].value<int>(), r, g, b);
            matrix->Show();
            return std::string("ok");
        });

    AddTool("self.led_matrix.fill",
        "用同一种颜色填充整块 LED 矩阵。颜色为 6 位十六进制 RRGGBB，全黑表示熄灭。",
        PropertyList({
            Property("color", kPropertyTypeString, std::string("000000"))
        }),
        [matrix](const PropertyList& properties) -> ReturnValue {
            std::string color = properties["color"].value<std::string>();
            uint8_t r = 0, g = 0, b = 0;
            if (!ParseHexColor(color, &r, &g, &b)) {
                return std::string("invalid color: expect 6-digit RRGGBB");
            }
            matrix->Fill(r, g, b);
            matrix->Show();
            return std::string("ok");
        });

    AddTool("self.led_matrix.clear",
        "熄灭整块 LED 矩阵（清空画面）。",
        PropertyList(),
        [matrix](const PropertyList& properties) -> ReturnValue {
            (void)properties;
            matrix->Clear();
            matrix->Show();
            return std::string("ok");
        });

    AddTool("self.led_matrix.draw",
        "一次性绘制整块 LED 矩阵。pixels 为宽度*高度 个像素的 RGB888 十六进制串"
        "（每个像素 6 位，行优先）。",
        PropertyList({
            Property("pixels", kPropertyTypeString, std::string(""))
        }),
        [matrix](const PropertyList& properties) -> ReturnValue {
            if (!matrix->DrawHexFrame(properties["pixels"].value<std::string>())) {
                return std::string("invalid pixels payload");
            }
            matrix->Show();
            return std::string("ok");
        });

    AddTool("self.led_matrix.get_frame",
        "读取 LED 矩阵当前画面，返回 RGB888 十六进制字符串。",
        PropertyList(),
        [matrix](const PropertyList& properties) -> ReturnValue {
            (void)properties;
            return matrix->Snapshot();
        });

    ESP_LOGI(TAG, "Pixel drawing tools registered (%dx%d)", matrix->width(), matrix->height());
}


void MCPServer::ParseCapability(const cJSON* capability){
    
  
}




void MCPServer::ParseMessage(const std::string& message){
    cJSON* json = cJSON_Parse(message.c_str());
    if(json == nullptr){
        ESP_LOGI(TAG,"Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}



void MCPServer::ParseMessage(const cJSON* json){
    auto version = cJSON_GetObjectItem(json,"jsonrpc");
    if(version==nullptr||!cJSON_IsString(version)||strcmp(version->valuestring,"2.0")!=0){
       ESP_LOGI(TAG,"Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }

    auto method = cJSON_GetObjectItem(json,"method");
    if(method==nullptr||!cJSON_IsString(method)){
        ESP_LOGI(TAG,"method error");
        return;
    }

    std::string method_str = method->valuestring;
    // MCP 的通知类消息（notifications/initialized 等）按协议不需要回复，
    // 也没有 params/id；原来只匹配了 "notifications"，导致
    // "notifications/initialized" 落到后面报 "Invalid params"。
    if(method_str.rfind("notifications", 0) == 0){
        return;
    }

    auto params = cJSON_GetObjectItem(json,"params");
    if(params==nullptr||!cJSON_IsObject(params)){
        ESP_LOGI(TAG, "Invalid params for method: %s", method_str.c_str());
        return;       
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGI(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }

    auto id_int=id->valueint;

    if(method_str == "initialize"){
        if(cJSON_IsObject(params)){
            auto capability = cJSON_GetObjectItem(params,"capabilities");
            if(capability!=nullptr&&cJSON_IsObject(capability)){
                ParseCapability(capability);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"serverInfo\":{\"name\":\"";
        message += BOARD_NAME;
        message += "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    }else if(method_str == "tools/list"){
        std::string cursor="";
        bool list_user_only_tools=false;
        if(params!=nullptr){
            auto cursor_item = cJSON_GetObjectItem(params,"cursor");
            if(cJSON_IsString(cursor_item)){    
                cursor=cursor_item->valuestring;
            }
            auto with_user_tools  = cJSON_GetObjectItem(params,"withUserTools");
            if(cJSON_IsBool(with_user_tools)){
                list_user_only_tools=with_user_tools->valueint==1;
            }
        }
        GetToolList(id_int, cursor, list_user_only_tools);
    }else if(method_str == "tools/call"){
        if(!cJSON_IsObject(params)){
            ESP_LOGI(TAG,"tools/call:Missing params");
            ReplyError(id_int,"Missing Params");
            return;
        }

        auto tool_name = cJSON_GetObjectItem(params,"name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGI(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }

        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGI(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }           
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);        
    }else {
        ESP_LOGI(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, std::string("Method not implemented: ") + method_str);
    }
}

void MCPServer::GetToolList(int id,const std::string& cursor,bool list_user_only_tools){
    const int max_payload_size = 8000;
    std::string json="{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it=tools_.begin();
    std::string next_cursor="";
    while(it!=tools_.end()){
        if(!found_cursor){
            if((*it)->name()==cursor){
                found_cursor=true;
            }else{
                it++;
                continue;
            }
        }

        if(!list_user_only_tools && (*it)->user_only()){
            ++it;
            continue;
        }

        std::string tool_json = (*it)->to_json()+",";
        if(json.length()+tool_json.length()+30>max_payload_size){
            next_cursor=(*it)->name();
            break;
        }

        json+=tool_json;
        it++;

    }
    
    if (json.back() == ',') {
        json.pop_back();
    }

    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGI(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if(next_cursor.empty()){
        json+="]}";
    }else{
        json+="],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    ReplyResult(id, json);
}


void MCPServer::DoToolCall(int id,const std::string& tool_name,const cJSON* tool_arguments){
    auto tool_iter = std::find_if(tools_.begin(),tools_.end(),[&tool_name](const McpTool* t){return t->name()==tool_name;});
    if(tool_iter==tools_.end()){
        ESP_LOGI(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    for(auto& argument : arguments){
        bool found = false;
        if(cJSON_IsObject(tool_arguments)){
            auto value = cJSON_GetObjectItem(tool_arguments,argument.name().c_str());
            if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                argument.set_value<bool>(value->valueint == 1);
                found = true;
            } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                argument.set_value<int>(value->valueint);
                found = true;
            } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                argument.set_value<std::string>(value->valuestring);
                found = true;
            }
        }

        if (!argument.has_default_value() && !found) {
            ESP_LOGI(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
            ReplyError(id, "Missing valid argument: " + argument.name());
            return;
        }
    }

    auto& app = Application::GetInstance();
    app.Schedule([this,id,tool_iter,arguments=std::move(arguments)]() mutable {
        ReplyResult(id, (*tool_iter)->Call(arguments));
    });
}

