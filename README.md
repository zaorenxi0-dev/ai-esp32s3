# ai-esp32s3 · 程序架构

ESP32-S3 语音助手固件的**程序架构说明**。整机采用分层设计：板级抽象层屏蔽硬件差异，
应用层用「单例 + 事件组 + 状态机」驱动主流程，能力层以服务对象的形式挂载音频、MCP、OTA，
协议层通过纯虚接口对接云端。WS2812 矩阵与 BLE 像素绘画作为外设驱动扩展接入同一套 MCP 工具总线。

- 目标芯片：**ESP32-S3**（8 MB Flash + PSRAM）
- SDK：**ESP-IDF v5.5.2**
- 主程序语言：**C++17**（`gnu++17`）

---

## 一、总体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│  应用层  Application（单例）                                          │
│  ├─ 主事件循环 Run()：EventGroup 等待 → 分发 13 个事件位               │
│  ├─ Schedule()：跨任务投递闭包到主任务（线程安全的唯一入口）           │
│  └─ 设备状态机 DeviceStateMachine：11 个状态 + 迁移合法性校验         │
├──────────────────────────────────────────────────────────────────────┤
│  能力层                                                              │
│  ├─ AudioService   音频采集/播放、Opus 编解码、AFE 前处理、唤醒词      │
│  ├─ MCPServer      JSON-RPC 工具注册与分发（云端 LLM 调用设备能力）    │
│  ├─ Ota            分块下载 → 备用分区 → 校验 → 切换启动分区          │
│  └─ Assets         提示音/语言包，独立 assets 分区加载                │
├──────────────────────────────────────────────────────────────────────┤
│  协议层  Protocol（抽象基类）                                         │
│  ├─ MqttProtocol   MQTT 走控制信令，UDP 走实时音频（AES-CTR）          │
│  └─ WebSocketProtocol                                                │
│     依赖 NetworkInterface 抽象：CreateMqtt / CreateUdp / CreateTcp /  │
│     CreateHttp，由板级实现具体创建                                    │
├──────────────────────────────────────────────────────────────────────┤
│  板级抽象层  Board（抽象基类，DECLARE_BOARD 注册）                     │
│  ├─ GetAudioCodec() / GetNetwork() / GetDisplay() / SetPowerSaveLevel │
│  ├─ WifiBoard：WiFi 事件、AP 配网、DNS 劫持、连接状态上报             │
│  └─ CompactWifiBoardLCD：本板 I2S 引脚与编解码器实例                   │
├──────────────────────────────────────────────────────────────────────┤
│  外设驱动层                                                          │
│  ├─ EasyMatrix  WS2812 矩阵（RMT TX + 自写编码器）                    │
│  ├─ BLE         NimBLE 自定义 GATT 服务（0xABF0 / 0xABF1）            │
│  └─ Sdcard / Display                                                 │
├──────────────────────────────────────────────────────────────────────┤
│  平台层  ESP-IDF · FreeRTOS · NimBLE · ESP-SR · mbedTLS · lwIP        │
└──────────────────────────────────────────────────────────────────────┘
```

**依赖方向单向向下**：应用层只依赖能力层与板级抽象，板级抽象只依赖协议层接口，
驱动层不反向依赖应用层——`EasyMatrix` / `BLE` 通过 `GetEasyMatrix()` 这类工厂函数被上层取用。

---

## 二、应用层：事件驱动的单线程主循环

`Application` 是进程内唯一实例（`GetInstance()` 静态局部变量），所有状态变更都收敛到主任务。

```cpp
void Application::Run() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ...);   // 阻塞等事件
        if (bits & MAIN_EVENT_SCHEDULE)            { /* 取出 main_tasks_ 队列逐个执行 */ }
        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED)  { HandleWakeWordDetectedEvent(); }
        if (bits & MAIN_EVENT_TOGGLE_CHAT)         { HandleToggleChatEvent(); }
        ...
    }
}
```

事件位（`application.h`）：

| 事件位 | 触发源 | 处理 |
|---|---|---|
| `MAIN_EVENT_SCHEDULE` | 任意任务调用 `Schedule()` | 执行投递的闭包 |
| `MAIN_EVENT_WAKE_WORD_DETECTED` | 唤醒词任务 | 进入 listening |
| `MAIN_EVENT_VAD_CHANGE` | 音频前处理 | 自动断句 |
| `MAIN_EVENT_SEND_AUDIO` | 上行编码完成 | 经协议发送 |
| `MAIN_EVENT_TOGGLE_CHAT` / `START_LISTENING` / `STOP_LISTENING` | 按键 / MCP | 手动对话控制 |
| `MAIN_EVENT_NETWORK_CONNECTED` / `DISCONNECTED` | 板级网络回调 | 建链 / 重连 |
| `MAIN_EVENT_ACTIVATION_DONE` / `STATE_CHANGED` / `ERROR` / `CLOCK_TICK` | 各服务 | 激活、状态广播、错误、时钟 |

**并发约束**：驱动回调、BLE 回调、音频任务都不直接改状态，而是 `Schedule()` 把闭包投到主任务；
`main_tasks_` 用 `std::mutex` 保护。这是整个固件避免竞态的核心约定。

---

## 三、状态层：显式迁移表

`DeviceStateMachine` 用 `std::atomic<DeviceState>` 存当前状态，迁移必须通过 `IsValidTransition()` 白名单：

```
unknown → starting → ┬→ wifi_configuring ⇄ audio_testing
                     └→ activating → ┬→ idle ⇄ connecting ⇄ listening ⇄ speaking
                                     └→ upgrading
idle → {connecting, listening, speaking, activating, upgrading, wifi_configuring}
fatal_error → （终态，不可迁出）
```

- 非法迁移只打日志并返回 `false`，不抛异常、不改状态
- 状态变化通过 `AddStateCallback()` 注册的监听器广播（先拷贝回调列表再调用，避免回调里增删监听导致迭代器失效）
- 上层用 `Application::SetDeviceState()` 统一入口，内部即走状态机校验

---

## 四、能力层

### 4.1 AudioService

内部由三个可插拔组件 + 收发队列组成：

| 组件 | 实现 | 职责 |
|---|---|---|
| `AudioCodec` | `NoAudioCodecSimplex` | I2S 采集/播放，16 kHz 输入 / 24 kHz 输出 |
| `AudioProcessor` | `AfeAudioProcessor` | AFE 回声消除、降噪、VAD |
| `WakeWord` | `AfeWakeWord` / `CustomWakeWord` | 端侧唤醒词检测 |

```
麦克风 → codec → audio_processor →┬→ 唤醒词任务（检测到 → MAIN_EVENT_WAKE_WORD_DETECTED）
                                 └→ 上行队列 → Opus 编码 → 协议发送
协议接收 → 下行队列 → Opus 解码 → codec → 扬声器
```

编码/解码分别跑在独立任务上，队列长度与阻塞策略决定端到端时延；`EnableWakeWordDetection()` /
`EnableVoiceProcessing()` 在对话态与待机态之间切换资源占用。

### 4.2 MCPServer

把设备能力以 **JSON-RPC 工具**的形式暴露给云端大模型：

```
云端 → MQTT/WebSocket 文本帧 → Protocol::OnIncomingJson → MCPServer::ParseMessage
     → 按 method 分发（initialize / tools/list / tools/call）
     → 命中工具回调 → ReturnValue → Application::SendMCPMessage 回包
```

工具注册三要素：`name`（如 `self.led_matrix.set_pixel`）、`description`（给模型看的自然语言说明）、
`PropertyList`（参数名/类型/取值范围，会生成 JSON Schema 供模型约束输出）。

### 4.3 Ota

`esp_ota_begin()` 在**读取数据流之前**调用，随后边收边写；收完 `esp_ota_end()` 校验镜像，
再 `esp_ota_set_boot_partition()` 切换。分区表为双 OTA（各 3 MB），升级失败可回滚。

---

## 五、板级抽象：一处实现，多处复用

`Board` 是纯虚基类，`DECLARE_BOARD(类名)` 宏在编译期把具体板型注册进工厂，
`Board::GetInstance()` 通过 `create_board()` 拿到唯一实例。新增板型只需：

1. 在 `main/boards/<board>/` 放 `config.h`（引脚、采样率）与 `<board>.cc`（继承 `WifiBoard`）
2. `GetAudioCodec()` 返回该板的 codec 实例
3. 文件末尾 `DECLARE_BOARD(YourBoard)`
4. `main/CMakeLists.txt` 的 include 路径与 `BOARD_TYPE` 指向该目录

应用层与协议层完全不需要改动。

---

## 六、外设扩展：矩阵与 BLE

### 6.1 EasyMatrix（WS2812）

不依赖外部 `led_strip` 组件，直接用**新版 RMT TX 驱动 + 自定义编码器**：

```
framebuffer_ (RGB888, 逻辑像素序)
   → Show()：叠加亮度、转 GRB、蛇形走线映射
   → RMT bytes_encoder（T0H/T0L、T1H/T1L）+ copy_encoder（>50 µs 复位码）
   → WS2812 灯珠
```

- `mem_block_symbols = 64`，一帧 8×8×24 bit 需要多次 `MEM_FULL` 续传，编码器状态机负责分段
- 所有像素操作持 `mutex_`；`Show()` 在锁外发送，避免长时间持锁
- 帧缓存与 RMT 句柄解耦，`Snapshot()` / `DrawHexFrame()` 构成一对互逆的序列化接口

### 6.2 BLE 像素绘画

```
手机/小程序 ──BLE Write──> GATT 0xABF1 ──> 分包重组（按连接隔离）
   → 整帧判定（帧头 7 字节含负载长度）
   → 校验（包头/命令字/宽高一致性/累加和）
   → 逐像素写 framebuffer → Show()
```

帧格式：

```
偏移  字段        长度   说明
0     HEAD        2     0xAA 0x55
2     CMD         1     0x02 = 绘制 RGB 帧
3     H           1     行数
4     W           1     列数
5     LEN_H/L     2     负载长度 = H*W*3（大端）
7     PAYLOAD     N     RGB888，行优先
7+N   CHECKSUM    1     从 CMD 累加到 PAYLOAD 末字节
```

NimBLE host 任务栈默认仅 4 KB，回调里不分配大缓冲：先探 2 字节帧头，再直接拷进重组缓存；
`CONFIG_BT_NIMBLE_MAX_CONNECTIONS > 1` 时按 `conn_handle` 隔离缓存，防止两路设备交错拼接。

---

## 七、任务与同步

| 任务 | 创建位置 | 职责 | 与其他任务的同步 |
|---|---|---|---|
| main | `application.cc` | 事件循环、状态迁移、业务编排 | EventGroup + `main_tasks_` 队列 |
| audio 输入/输出 | `audio_service.cc` | I2S 采播、前处理 | 队列 + EventGroup |
| Opus 编码/解码 | `audio_service.cc` | 编解码 | 收发队列 |
| 唤醒词 | `audio_service.cc` | AFE 唤醒检测 | 置事件位 |
| activation | `application.cc` | 首次配网激活流程 | 置 `MAIN_EVENT_ACTIVATION_DONE` |
| NimBLE host | `ble.cc` | GATT 事件、分包重组 | 全局重组缓存（按连接隔离） |
| AP 配网 / DNS | `wifi_configuration_ap.cc` | 热点、DNS 劫持 | 回调上报 |

**约定**：跨任务只传「事件位」或「闭包」，共享数据一律加锁；ISR/驱动回调里不做耗时操作。

---

## 八、目录与职责

```
main/
├── ai-esp32s3.cpp            入口：初始化板级 → Application::Initialize/Run
├── application.cc/.h         应用层：事件循环、状态迁移、业务编排   ★ 架构核心
├── device_state_machine.cc   状态层：状态与迁移白名单
├── device_state.h            状态枚举
├── audio/                    能力层：AudioService / codec / processor / wake_word
├── protocols/                协议层：Protocol 抽象、MQTT、WebSocket、网络封装
├── wifi/                     板级：WiFi 事件、AP 配网页面、DNS
├── boards/                   板级：Board 抽象 + 具体板型
├── display/                  显示抽象（当前为 NoDisplay）
├── drivers/                  SD 卡等外设
├── ota.cc                    能力层：OTA 升级
├── mcp_server.cc/.h          能力层：MCP 工具注册与 JSON-RPC 分发
├── assets.cc                 资源：提示音、语言包
├── easymatrix.cc/.h   ★      驱动：WS2812 矩阵
└── ble.cc/.h          ★      驱动：BLE 像素绘画通道
```

---

## 九、硬件与分区

| 用途 | 引脚 |
|---|---|
| 麦克风 I2S（SCK / WS / DIN） | GPIO5 / GPIO4 / GPIO6 |
| 扬声器 I2S（BCLK / LRCK / DOUT） | GPIO15 / GPIO16 / GPIO7 |
| WS2812 数据线 | GPIO10（`MATRIX_GPIO` 可改） |

矩阵尺寸与走线方式在 `main/easymatrix.h` 用宏配置：

```c
#define MATRIX_GPIO        10
#define MATRIX_WIDTH       8
#define MATRIX_HEIGHT      8
#define MATRIX_SERPENTINE  1   // 1 = 蛇形走线，0 = 逐行同向
```

8 MB Flash 分区（双 OTA，可回滚）：

```
nvs        0x009000    16 KB
otadata    0x00D000     8 KB
phy_init   0x00F000     4 KB
ota_0      0x020000     3 MB
ota_1      0x310000     3 MB
assets     0x600000  1.875 MB
```

---

## 十、构建

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

首次构建会按 `dependencies.lock` 拉取 `managed_components/`（约 280 MB，不入库）。

**Windows / Git Bash**：Git Bash 会注入 `MSYSTEM`，`idf.py` 检测到即跳过 `main()`，且 `env -u`
无效（会被重新注入）。可靠做法是在 Python 进程内清掉后再以 `__main__` 方式启动：

```bash
python -c "
import os,runpy,sys
os.environ.pop('MSYSTEM',None); os.environ.pop('MSYS',None)
tools=os.path.join(os.environ['IDF_PATH'],'tools'); sys.path.insert(0,tools)
sys.argv=['idf.py','build']; runpy.run_path(os.path.join(tools,'idf.py'),run_name='__main__')
"
```

或直接用 **ESP-IDF 官方命令提示符**。`tools_idf_env.sh` 仅用于导出工具链路径，需按本机修改。

### 部署配置（必读）

本仓库不含可用的服务器地址，需自行部署 [xiaozhi-esp32 服务端](https://github.com/xinnan-tech/xiaozhi-esp32-server) 后配置：

| 配置项 | 位置 | 说明 |
|---|---|---|
| `CONFIG_XIAOZHI_OTA_URL` | `sdkconfig.defaults` / `idf.py menuconfig` | OTA 与激活服务地址，默认占位 `http://your-server-ip:8002/xiaozhi/ota/` |
| WiFi 配网页面默认 OTA 地址 | `main/wifi/wifi_configuration.html` | 占位同理，按需修改 |

`managed_components/`、`build/`、`sdkconfig`、`*.log` 均不入库：本地 `sdkconfig` 含个人服务器地址，
配置基线以脱敏后的 `sdkconfig.defaults` 为准（删除本地 `sdkconfig` 后 `idf.py reconfigure` 会自动重建）。

---

## 十一、已知限制

- BLE 单帧负载上限 `FRAME_MAX_LEN - 8 = 1016` 字节，8×8 面板可一次成型，更大面板需上层分帧
- `Show()` 同步等待 RMT 发送完成（超时 1 s），连续高频绘制需上层节流
- 亮度只作用于后续 `Show()`，不改变帧缓存内容
- 显示层当前为 `NoDisplay`，LCD 驱动未启用
- 编译存在 `-Wmissing-field-initializers` 与上游 `audio_codec.h` 的 `TAG` 重定义告警，不影响功能

---

## 十二、致谢

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) —— 语音交互程序框架与整体架构设计
- [ESP-IDF](https://github.com/espressif/esp-idf) —— 官方 SDK
