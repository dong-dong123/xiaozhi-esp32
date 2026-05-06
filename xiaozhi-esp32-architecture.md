# xiaozhi-esp32 完整项目代码流程图

> 小智AI助手 — ESP32系列芯片智能语音助手固件

---

## 一、系统启动全流程

```
app_main()  [main.cc:27]
│
├─ nvs_flash_init()                           // 初始化NVS闪存
├─ Application::GetInstance().Initialize()    [application.cc]
│  │
│  ├─ SetDeviceState(kDeviceStateStarting)    // 状态: 启动中
│  ├─ Board::GetInstance().SetupUI()          // 初始化Display/Backlight
│  ├─ AudioService::Initialize(codec)         // 获取板载AudioCodec, 创建3个任务
│  │     ├─ AudioInputTask                    // MIC采集 → 处理器/唤醒词
│  │     ├─ AudioOutputTask                   // 解码队列 → 扬声器
│  │     └─ OpusCodecTask                     // Opus编解码
│  ├─ AudioService::Start()                   // 启动音频任务
│  ├─ AudioService::SetCallbacks()            // 注册VAD/唤醒词回调
│  ├─ StateMachine::AddStateChangeListener()  // 注册状态变化监听
│  ├─ 启动1秒周期时钟定时器                     // MAIN_EVENT_CLOCK_TICK
│  ├─ McpServer::AddCommonTools()             // 注册MCP通用工具(AI可见)
│  ├─ McpServer::AddUserOnlyTools()           // 注册用户工具(AI不可见)
│  ├─ Board::SetNetworkEventCallback()        // 网络事件回调
│  └─ Board::StartNetwork()                   // 异步启动网络(无阻塞)
│
├─ Application::Run()                         // 永不返回的主事件循环
│  │
│  └─ while(true) {
│       bits = xEventGroupWaitBits(ALL_13_EVENTS)
│       switch(bits) {
│         case CLOCK_TICK:       每秒心跳/电量/OTA检查
│         case SEND_AUDIO:       发送编码后的音频到服务器
│         case WAKE_WORD:        唤醒词检测 → 开始录音
│         case VAD_CHANGE:       语音活动检测
│         case ERROR:            网络错误处理/重连
│         case NETWORK_CONNECTED:   → Activating → Idle
│         case NETWORK_DISCONNECTED: → 重连/配网模式
│         case TOGGLE_CHAT:      按键触发 → 开始/停止对话
│         case START_LISTENING:  MCP命令开始监听
│         case STOP_LISTENING:   MCP命令停止监听
│         case ACTIVATION_DONE:  激活完成 → Idle
│         case STATE_CHANGED:    通知LED/Display更新
│         case SCHEDULE:         执行入队的回调任务
│       }
│     }
```

---

## 二、完整音频数据流

### 2.1 上行音频 (MIC → 云)

```
  [硬件MIC]
     │
     ▼
  ┌─────────────┐    I2S     ┌───────────────┐    PCM    ┌─────────────────────┐
  │ AudioCodec  │───────────▶│ AudioService  │──────────▶│ AudioProcessor       │
  │ (ES8311/    │  16bit     │ AudioInputTask│           │ (AfeAudioProcessor)  │
  │  ES8388/    │  1-4ch     │               │           │  ├─ AEC 回声消除      │
  │  NoCodec)   │            │               │           │  ├─ NS  噪声抑制      │
  └─────────────┘            └───────────────┘           │  └─ VAD 语音活动检测  │
                                                          └─────────┬───────────┘
                                                                    │
                        ┌──────────────────┐                        │
                        │   WakeWord       │◄── 原始PCM(可选)       │
                        │ (AFE/Custom/ESP) │                        │
                        │   唤醒词检测      │                        ▼
                        └──────────────────┘           ┌─────────────────────┐
                            │ 检测到唤醒词              │   环形缓冲           │
                            ▼                           │   (encode_queue_)   │
                     MAIN_EVENT_WAKE_WORD               └─────────┬───────────┘
                                                                  │
                                                                  ▼
                                                      ┌─────────────────────┐
                                                      │ OpusCodecTask       │
                                                      │ (Opus编码, ~32kbps) │
                                                      └─────────┬───────────┘
                                                                │
                                                                ▼
                                ┌──────────────────────────────────────────┐
                                │             发送队列 (send_queue_)        │
                                └────────────────────┬─────────────────────┘
                                                     │
                              MAIN_EVENT_SEND_AUDIO  │
                                                     ▼
                    ┌──────────────────────────────────────────────────┐
                    │  Protocol::SendAudio()                           │
                    │  ├─ MQTT: UDP通道 + AES-CTR加密                   │
                    │  └─ WebSocket: Binary帧                          │
                    └────────────────────┬─────────────────────────────┘
                                         │
                                         ▼
                                   ☁️  云端服务器
```

### 2.2 下行音频 (云 → 扬声器)

```
                              ☁️ 云端服务器
                                 │
                    ┌────────────┴────────────┐
                    ▼                         ▼
           ┌──────────────┐          ┌──────────────┐
           │  TTS 音频流   │          │  JSON 信令    │
           │  (Opus编码)   │          │ (STT/LLM/MCP)│
           └──────┬───────┘          └──────┬───────┘
                  │                         │
                  ▼                         ▼
     ┌──────────────────┐       ┌──────────────────┐
     │  Decode Queue    │       │ McpServer::      │
     │  (decode_queue_) │       │ ParseMessage()   │
     └────────┬─────────┘       │ → tools/list     │
              │                 │ → tools/call     │
              ▼                 └──────────────────┘
     ┌──────────────────┐
     │ OpusCodecTask     │
     │ (Opus解码→PCM)   │
     └────────┬─────────┘
              │
              ▼
     ┌──────────────────┐
     │ Playback Queue   │
     │ (playback_queue_)│
     └────────┬─────────┘
              │
              ▼
     ┌──────────────────┐
     │ AudioOutputTask   │
     │ PCM → I2S → 功放  │
     └────────┬─────────┘
              │
              ▼
     ┌──────────────────┐
     │ AudioCodec::Write │
     │ → ES8311 DAC      │
     │ → PWM/MAX98357    │
     └──────────────────┘
```

---

## 三、设备状态机完整转移图

```
                    ┌──────────┐
                    │  Unknown │
                    └────┬─────┘
                         │ Initialize()
                         ▼
              ┌────────────────────┐
              │     Starting       │────── 按键进入配网 ──────┐
              └────────┬──────────┘                          │
                       │                                     │
         网络连接      │  无WiFi凭证                          │
                       ▼                                     ▼
              ┌──────────────────┐              ┌──────────────────────┐
              │   Activating     │              │  WifiConfiguring     │◄────┐
              │ (OTA/版本检查)    │              │ (AP/BluFi/声波配网)  │     │
              └────┬───┬────┬────┘              └──────┬───────┬───────┘     │
                   │   │   │                           │       │             │
       有新固件   │   │   │ 激活完成                   │ 音频测试│             │
                   ▼   │   ▼                           ▼       │             │
        ┌──────────┐  │  ┌──────────┐   ┌──────────────────┐  │             │
        │Upgrading │  │  │   Idle   │◄──│ AudioTesting     │──┘             │
        │(OTA升级) │  │  │ (空闲)   │   └──────────────────┘                │
        └────┬─────┘  │  └─┬──┬──┬─┘                                       │
             │        │    │  │  │                                          │
             ▼        │    │  │  └──── 按键/WiFi配网 ────────────────────────┘
          重启        │    │  │
                      │    │  └──── OTA检查 ──→ Upgrading
                      │    │
        ┌─────────────┘    │
        │                  │ 按键/唤醒词/MCP命令
        │                  ▼
        │        ┌──────────────────┐
        │        │   Connecting     │──── 失败 ──→ Idle
        │        │  (连接服务器)     │
        │        └────────┬─────────┘
        │                 │ ServerHello成功
        │                 ▼
        │        ┌──────────────────┐
        │        │   Listening      │◄──────────────────┐
        │        │  (聆听用户语音)   │                   │
        │        └────┬────────┬────┘                   │
        │             │        │                        │
        │   TTS开始   │        │ 静音超时/手动停止       │
        │             ▼        ▼                        │
        │        ┌──────────┐  ┌──────┐                │
        │        │ Speaking │  │ Idle │                │
        │        │(播放TTS) │  └──────┘                │
        │        └────┬─────┘                          │
        │             │                                │
        │  TTS结束(AutoStop模式) ───────────────────────┘
        │  TTS结束(ManualStop模式) ──→ Idle
        │
        └──────────── FatalError (不可恢复)
```

### 状态转移合法性验证表

| 当前状态 ↓ / 目标状态 → | Unknown | Starting | WifiConfig | Idle | Connecting | Listening | Speaking | Upgrading | Activating | AudioTesting | FatalError |
|------------------------|---------|----------|------------|------|------------|-----------|----------|-----------|-------------|--------------|------------|
| **Unknown**            |         | ✅        |            |      |            |           |          |           |             |              |            |
| **Starting**           |         |          | ✅          | ✅    |            |           |          |           | ✅           |              |            |
| **WifiConfiguring**    |         |          |            | ✅    |            |           |          |           | ✅           | ✅            |            |
| **AudioTesting**       |         |          | ✅          |      |            |           |          |           |             |              |            |
| **Activating**         |         |          | ✅          | ✅    |            |           |          | ✅         |             |              |            |
| **Upgrading**          |         |          |            | ✅    |            |           |          |           | ✅           |              |            |
| **Idle**               |         |          | ✅          |      | ✅          |           |          | ✅         | ✅           |              |            |
| **Connecting**         |         |          |            | ✅    |            | ✅         |          |           |             |              |            |
| **Listening**          |         |          |            | ✅    |            |           | ✅        |           |             |              |            |
| **Speaking**           |         |          |            | ✅    |            | ✅         |          |           |             |              |            |
| **FatalError**         |         |          |            |      |            |           |          |           |             |              |            |

---

## 四、类继承体系架构

### 4.1 全局单例体系

```
Application::GetInstance()          Board::GetInstance()
│                                   │ (DECLARE_BOARD宏/工厂模式)
│                                   │
├── DeviceStateMachine              ├── WifiBoard ─── EspBox3Board
│   (std::atomic + 观察者)          │   (WiFi)        (esp-box-3)
│                                   │
├── AudioService                    ├── Ml307Board ── BreadCompactMl307
│   ├── AudioCodec*                 │   (4G蜂窝)      (bread-compact)
│   ├── AudioProcessor*             │
│   ├── WakeWord*                   ├── DualNetworkBoard
│   └── AudioDebugger*              │   (WiFi+4G双网)
│                                   │
├── Protocol*                       └── RndisBoard
│   ├── MqttProtocol                    (USB网络共享)
│   └── WebsocketProtocol           【~90个具体板卡】
│
└── Ota* (创建→使用→销毁)

Assets::GetInstance()               McpServer::GetInstance()
├── LvglStrategy / EmoteStrategy     ├── common_tools_
└── 字体/Emoji/SR模型管理             └── user_only_tools_
```

### 4.2 Audio 编解码器继承体系

```
                          AudioCodec (抽象基类)
                          │  Read() / Write()
                          │  SetOutputVolume()
                          │  EnableInput/Output()
                          │
        ┌─────────┬───────┼───────────┬──────────┬──────────┐
        ▼         ▼       ▼           ▼          ▼          ▼
  BoxAudioCodec Es8311  Es8388     Es8389     Es8374   NoAudioCodec
  (ES8311+      Audio   Audio      Audio      Audio    ├ Duplex
   ES7210)      Codec   Codec      Codec      Codec    ├ Simplex
  官方乐鑫板卡  ~35+板  ~5板      ~2板       1板       └ SimplexPdm
  (esp-box等)   (最常用) (ATK等)  (ATKBOX2)  (mixgo)   ~25+ DIY板
```

### 4.3 Display 显示系统继承体系

```
                            Display (抽象)
                            │
              ┌─────────────┼──────────────┐
              ▼             ▼              ▼
         NoDisplay     LvglDisplay    EmoteDisplay
         (无显示)        │            (表情动画, 非LVGL引擎)
                        │
             ┌──────────┼──────────┬──────────┐
             ▼          ▼          ▼          ▼
        LcdDisplay  OledDisplay  ...       ...
        ├ SpiLcd     (SSD1306/
        ├ RgbLcd      SH1106)
        └ MipiLcd

  LvglDisplay 包含:
  ├── 状态栏 (电池/网络/静音/时间)
  ├── 通知弹窗
  ├── Emoji表情 (Twemoji32/64, 21种)
  ├── GIF动画控制器
  ├── 主题管理器 (Light/Dark)
  └── 截图转JPEG
```

### 4.4 Audio Processor + Wake Word 继承体系

```
  AudioProcessor (抽象)               WakeWord (抽象)
  ├── AfeAudioProcessor               ├── AfeWakeWord
  │   ├── AEC (回声消除)              │   (AFE + WakeNet)
  │   ├── NS  (噪声抑制)              ├── CustomWakeWord
  │   └── VAD (语音活动检测)          │   (MultiNet, 自定义命令)
  └── NoAudioProcessor                └── EspWakeWord
      (透传, 不做处理)                    (轻量WakeNet)
```

### 4.5 Board 板级抽象体系

```
Board (抽象)
├── GetAudioCodec()        = 0 (纯虚函数)
├── GetDisplay()           可选
├── GetBacklight()         可选
├── GetLed()               可选
├── GetCamera()            可选
├── GetBatteryLevel()      可选
├── StartNetwork()         纯虚
└── GetBoardJson()         纯虚

Concrete Boards:
  ├── WifiBoard              WiFi板卡 (~60+ 板卡)
  ├── Ml307Board             4G蜂窝板卡 (~10+ 板卡)
  ├── Nt26Board              4G NT26板卡
  ├── DualNetworkBoard       双网(WiFi+4G)板卡
  └── RndisBoard             USB网络共享板卡
```

### 4.6 LED 指示灯继承体系

```
Led (抽象) ── OnStateChanged(state)
├── SingleLed         单颗WS2812 RGB
├── CircularStrip     WS2812环形灯带(多LED)
├── GpioLed           普通GPIO (PWM呼吸效果)
└── NoLed             无LED(空实现)
```

---

## 五、网络连接与协议选择流程

```
Board::StartNetwork()
│
├── WifiBoard (WiFi板卡)
│   ├── 已保存WiFi凭证 → 直接连接STA
│   │   └── 60秒超时 → 失败则进入配网模式
│   └── 无WiFi凭证 → 1.5秒后进入配网模式
│       ├── 热点配网 (AP模式, HTTP配置页)
│       ├── BluFi配网 (蓝牙配网)
│       └── 声波配网 (音频传输凭证)
│
├── Ml307Board (4G板卡)
│   └── AT命令检测modem → SIM卡检查 → 网络注册
│
└── DualNetworkBoard (双网板卡)
    └── WiFi优先, 可运行时切换至4G

网络连接成功 → MAIN_EVENT_NETWORK_CONNECTED
│
└── HandleNetworkConnectedEvent()
    └── SetDeviceState(kDeviceStateActivating)
        └── xTaskCreate(ActivationTask)
            │
            ├── Ota::CheckVersion()
            │   ├── HTTP POST → OTA服务器
            │   │   请求头: Device-Id, Client-Id(UUID), Activation-Version
            │   │   请求体: 系统信息JSON (MAC/版本/芯片/etc)
            │   │
            │   └── 解析响应:
            │       ├── firmware: {version, url} → 有新版本则升级
            │       ├── mqtt: {endpoint, client_id, ...} → 存NVS("mqtt")
            │       ├── websocket: {url, token, ...} → 存NVS("websocket")
            │       ├── activation: {code, challenge} → 激活流程
            │       └── server_time: {timestamp} → 同步系统时间
            │
            ├── CheckAssetsVersion() → 检查资源分区更新
            │
            └── InitializeProtocol()
                ├── NVS("mqtt") 有配置 → MqttProtocol
                └── NVS("websocket") 有配置 → WebsocketProtocol
```

### 通信协议对比

| 特性 | MQTT | WebSocket |
|------|------|-----------|
| 信令通道 | MQTT Publish | WebSocket Text |
| 音频通道 | UDP (AES-CTR加密) | WebSocket Binary |
| 二进制协议版本 | V3 | V1/V2/V3 |
| 重连机制 | 60秒自动重连 (仅Idle状态) | 无 (断开即结束) |
| 超时检测 | 120秒无数据 | 依赖WS keepalive |
| 配置存储 | NVS("mqtt") | NVS("websocket") |
| MCP支持 | ✅ (features.mcp) | ✅ (features.mcp) |
| AEC支持 | ✅ (features.aec) | ✅ (features.aec) |

---

## 六、MCP Server 工具注册与调用流程

### 6.1 MCP Server 初始化

```
  McpServer::AddCommonTools()      McpServer::AddUserOnlyTools()
  (AI大模型可见)                    (仅用户可见, AI不可见)
  ─────────────────────────────────────────────────────
  self.get_device_status            self.get_system_info
  self.audio_speaker.set_volume     self.reboot
  self.screen.set_brightness        self.upgrade_firmware
  self.screen.set_theme             self.screen.get_info
  self.camera.take_photo            self.screen.snapshot
                                    self.screen.preview_image
                                    self.assets.set_download_url

  板卡自定义MCP工具 (各板Initialize中添加):
  self.set_press_to_talk (press_to_talk_mcp_tool)
  self.lamp.toggle        (lamp_controller MCP demo)
```

### 6.2 MCP 工具调用流程

```
  ☁️ 服务器 → JSON-RPC 2.0
  {"method": "tools/call", "params": {"name": "self.xxx", ...}}
       │
       ▼
  Protocol::OnIncomingJson() → McpServer::ParseMessage()
       │
       ├── 参数验证 (类型检查/范围检查/必填检查)
       ├── Application::Schedule() → 投递到主线程
       │
       ▼
  MAIN_EVENT_SCHEDULE → McpTool::Call()
       │
       └── 通过 Protocol::SendMcpMessage() 返回结果/错误

  分页机制: tools/list 最大8000字节/页, 支持cursor游标
  Capabilities协商: 服务端传递vision能力(上传URL+Token)
```

---

## 七、OTA固件升级流程

```
有新版本可用
│
├── 下载阶段
│   ├── esp_ota_get_next_update_partition()  // 获取OTA分区
│   ├── HTTP GET 固件URL (流式下载)
│   │   └── 进度回调 → Display::SetChatMessage()
│   ├── 4KB缓冲页写入Flash
│   │   └── esp_ota_write() (含SHA256校验)
│   └── 下载速度/百分比计算 (每秒)
│
├── 校验阶段
│   ├── esp_ota_end() // 校验完整固件
│   └── 比较版本号 (当前 vs 新版本)
│
├── 激活阶段
│   ├── esp_ota_set_boot_partition()  // 设置下次启动分区
│   └── Application::Reboot()
│
└── 重启后自动运行新固件

设备激活 (有序列号 + 挑战码):
  eFuse_Key0 + challenge → HMAC-SHA256 → POST /api/activate
```

---

## 八、编译时配置系统 (Kconfig → menuconfig)

```
Xiaozhi Assistant (顶层菜单)
│
├── OTA URL ─────────────────────── 默认: https://api.tenclass.net/xiaozhi/ota/
├── Flash Assets ────────────────── 无/默认/自定义/表情资源
├── Language ────────────────────── 38种语言 (zh-CN, en-US, ja-JP, ...)
├── Board Type ──────────────────── ~90种板卡选择
│   └── 决定芯片目标: ESP32/S3/C3/C5/C6/P4
│
├── Display Type ────────────────── OLED/LCD (ST7789/ILI9341/GC9A01/...)
├── Display Style ───────────────── 默认/微信聊天/表情动画
│
├── Wake Word ───────────────────── 禁用/ESP WakeNet/AFE/自定义MultiNet
├── Audio Processing ────────────── 音频处理器 + AEC (设备端/服务端)
├── Audio Debugger ──────────────── UDP音频调试流
│
├── WiFi Config Method ──────────── 热点配网/声波配网/BluFi蓝牙配网
├── Camera Support ──────────────── JPEG输入/硬件编解码/旋转
│
└── Advanced Options ────────────── 唤醒词数据上传/多行消息/自定义消息

编译产物:
  CMakeLists.txt 根据 CONFIG_* 条件编译
    ├── Board: boards/<TYPE>/ 或 boards/<MFG>/<TYPE>/
    ├── AudioProcessor: afe_audio_processor / no_audio_processor
    ├── WakeWord: afe_wake_word + custom_wake_word / esp_wake_word
    ├── BluFi: blufi.cpp (条件)
    └── Language: gen_lang.py → lang_config.h (OGG音效嵌入)
```

---

## 九、LED 指示灯状态映射

|   设备状态    |  SingleLed (WS2812) |  CircularStrip (WS2812灯带) |   GpioLed (PWM GPIO)  |
|-------------|--------------------|----------------------------|-----------------------|
| Starting     | 蓝色快闪 100ms      | 蓝白滚动3颗灯               | PWM 50%闪烁 100ms     |
| WifiConfig   | 蓝色慢闪 500ms      | 蓝色全闪 500ms             | PWM 50%慢闪 500ms     |
| Idle         | 熄灭                | 渐暗熄灭                    | PWM 5%常亮            |
| Connecting   | 蓝色常亮            | 蓝色全亮                    | PWM 50%常亮           |
| Listening    | 红(有声高亮/无声低) | 红色全亮                    | PWM呼吸效果(10%/100%) |
| Speaking     | 绿色常亮            | 绿色全亮                    | PWM 75%常亮           |
| Upgrading    | 绿色快闪 100ms      | 绿色闪烁 100ms             | PWM 25%闪烁 100ms     |
| Activating   | 绿色慢闪 500ms      | 绿色慢闪 500ms             | PWM 35%慢闪 500ms     |

---

## 十、项目关键数据流总览

```
                        ┌─────────────────┐
                        │   ☁️ 云端服务     │
                        │  (MQTT/WebSocket)│
                        └──┬───────┬──────┘
                           │       │
               ┌───────────┘       └──────────┐
               ▼                               ▼
    ┌─────────────────┐             ┌─────────────────┐
    │   音频数据流      │             │   JSON 信令流    │
    │  (Opus编码)      │             │                 │
    │  UDP/WS Binary   │             │ STT/LLM/MCP     │
    │                  │             │ /TTS控制/System │
    └────────┬────────┘             └────────┬────────┘
             │                               │
             ▼                               ▼
    ┌─────────────────┐             ┌─────────────────┐
    │  AudioService   │             │    McpServer     │
    │  ├ Opus编解码    │             │  ├ tools/list    │
    │  ├ 音频输入/输出 │             │  ├ tools/call    │
    │  ├ 音频处理器    │             │  └ capabilities  │
    │  └ 唤醒词检测    │             └─────────────────┘
    └────────┬────────┘
             │
    ┌────────┴────────┐
    ▼                 ▼
┌───────┐      ┌──────────┐
│  MIC  │      │  Speaker │
└───────┘      └──────────┘

        ┌───────────────────────┐
        │    Application (主控)  │
        │    13事件驱动循环      │
        │  ┌─────────────────┐  │
        │  │ DeviceStateMachine│  │
        │  │ 11状态 + 转移验证 │  │
        │  └─────────────────┘  │
        └───────┬───────────────┘
                │
    ┌───────────┼───────────┐
    ▼           ▼           ▼
┌───────┐ ┌────────┐ ┌──────────┐
│Display│ │  LED   │ │  Button  │
│(LCD/  │ │(WS2812 │ │(GPIO/ADC │
│ OLED) │ │ /PWM)  │ │ /Knob)   │
└───────┘ └────────┘ └──────────┘
```

---

## 十一、目录结构总览

```
xiaozhi-esp32/
├── main/
│   ├── main.cc                         # 入口点 app_main()
│   ├── application.h / .cc             # 应用主控 (单例, 事件循环)
│   ├── device_state.h                  # 设备状态枚举
│   ├── device_state_machine.h / .cc    # 状态机 (转移验证+观察者)
│   ├── system_info.h / .cc             # 系统信息工具类
│   ├── ota.h / .cc                     # OTA升级和激活
│   ├── mcp_server.h / .cc             # MCP Server (JSON-RPC 2.0)
│   ├── settings.h / .cc                # NVS键值存储封装
│   ├── assets.h / .cc                  # 资源分区管理
│   ├── CMakeLists.txt                  # 构建系统 (条件编译)
│   ├── Kconfig.projbuild               # 所有配置选项定义
│   │
│   ├── protocols/                      # 通信协议层
│   │   ├── protocol.h / .cc            #   协议抽象基类
│   │   ├── mqtt_protocol.h / .cc       #   MQTT协议 (+UDP音频通道+AES加密)
│   │   └── websocket_protocol.h / .cc  #   WebSocket协议
│   │
│   ├── audio/                          # 音频子系统
│   │   ├── audio_codec.h / .cc         #   AudioCodec抽象基类
│   │   ├── audio_service.h / .cc       #   音频服务中心 (3个FreeRTOS任务)
│   │   ├── audio_processor.h           #   AudioProcessor抽象接口
│   │   ├── wake_word.h                 #   WakeWord抽象接口
│   │   ├── codecs/                     #   音频编解码器实现
│   │   │   ├── box_audio_codec.cc      #     ES8311+ES7210组合 (官方乐鑫板)
│   │   │   ├── es8311_audio_codec.cc   #     ES8311 (最常用, ~35+板)
│   │   │   ├── es8374_audio_codec.cc   #     ES8374
│   │   │   ├── es8388_audio_codec.cc   #     ES8388
│   │   │   ├── es8389_audio_codec.cc   #     ES8389
│   │   │   ├── no_audio_codec.cc       #     无外部芯片 (直连I2S)
│   │   │   └── dummy_audio_codec.cc    #     测试桩
│   │   ├── wake_words/                 #   唤醒词实现
│   │   │   ├── afe_wake_word.cc        #     AFE+WakeNet
│   │   │   ├── custom_wake_word.cc     #     MultiNet自定义
│   │   │   └── esp_wake_word.cc        #     轻量WakeNet
│   │   ├── processors/                 #   音频处理器实现
│   │   │   ├── afe_audio_processor.cc  #     AFE (AEC+NS+VAD)
│   │   │   ├── audio_debugger.cc       #     UDP调试流
│   │   │   └── no_audio_processor.cc   #     透传
│   │   └── demuxer/                    #   OGG解复用器
│   │       └── ogg_demuxer.cc          #     解析OGG音效文件
│   │
│   ├── display/                        # 显示子系统
│   │   ├── display.h / .cc             #   Display抽象基类
│   │   ├── lcd_display.h / .cc         #   LCD显示 (SPI/RGB/MIPI)
│   │   ├── oled_display.h / .cc        #   OLED显示 (SSD1306/SH1106)
│   │   ├── emote_display.h / .cc       #   表情动画显示 (非LVGL)
│   │   └── lvgl_display/               #   LVGL中间层
│   │       ├── lvgl_display.cc         #     状态栏/通知/电池/网络/截图
│   │       ├── lvgl_theme.cc           #     主题系统 (Light/Dark)
│   │       ├── lvgl_font.cc            #     字体封装
│   │       ├── lvgl_image.cc           #     图片封装
│   │       ├── emoji_collection.cc     #     Emoji集合 (21种表情)
│   │       ├── gif/                    #     GIF动画解码
│   │       └── jpg/                    #     JPEG编解码
│   │
│   ├── led/                            # LED子系统
│   │   ├── led.h                       #   Led抽象接口
│   │   ├── single_led.cc               #   单颗WS2812
│   │   ├── circular_strip.cc           #   WS2812环形灯带
│   │   └── gpio_led.cc                 #   PWM GPIO LED
│   │
│   ├── boards/                         # 板卡层 (~90+板卡)
│   │   ├── common/                     #   公共基础类
│   │   │   ├── board.h / .cc           #     Board抽象基类
│   │   │   ├── wifi_board.h / .cc      #     WiFi板卡
│   │   │   ├── ml307_board.h / .cc     #     4G ML307板卡
│   │   │   ├── nt26_board.h / .cc      #     4G NT26板卡
│   │   │   ├── dual_network_board.cc   #     双网板卡
│   │   │   ├── rndis_board.cc          #     USB网络共享
│   │   │   ├── button.h                #     按键封装
│   │   │   ├── knob.h / .cc            #     旋转编码器
│   │   │   ├── backlight.h / .cc       #     背光控制
│   │   │   ├── camera.h               #     摄像头抽象
│   │   │   ├── esp32_camera.cc         #     ESP32摄像头实现
│   │   │   ├── esp_video.cc            #     视频流
│   │   │   ├── adc_battery_monitor.cc  #     电池电量检测
│   │   │   ├── axp2101.cc              #     AXP2101 PMIC驱动
│   │   │   ├── sy6970.cc               #     SY6970 PMIC驱动
│   │   │   ├── lamp_controller.cc      #     MCP灯具控制示例
│   │   │   ├── press_to_talk_mcp_tool.cc #  按讲模式MCP工具
│   │   │   ├── power_save_timer.cc     #     CPU变频+休眠定时器
│   │   │   ├── sleep_timer.cc          #     深度睡眠定时器
│   │   │   ├── system_reset.cc         #     系统重置
│   │   │   ├── afsk_demod.cc           #     声波WiFi配网解调
│   │   │   └── blufi.cpp               #     BluFi蓝牙配网
│   │   │
│   │   ├── esp-box-3/                  #   ESP-BOX-3板卡
│   │   │   ├── config.h                #     引脚/采样率/显示配置
│   │   │   └── esp_box3_board.cc       #     板卡实现
│   │   ├── esp-box/                    #   ESP-BOX板卡
│   │   ├── esp-s3-lcd-ev-board/        #   ESP32-S3 LCD开发板
│   │   ├── bread-compact-esp32/        #   面包板ESP32
│   │   ├── atk-dnesp32s3-box3/         #   安信可DNESP32S3-BOX3
│   │   ├── waveshare/                  #   微雪系列 (15+子板卡)
│   │   ├── atoms3-echo-base/           #   M5Stack Atom Echo
│   │   └── ... (~90个板卡目录)
│   │
│   └── assets/                         # 资源管理
│       ├── common/                     #   通用音效 (OGG格式)
│       └── locales/                    #   38种语言本地化
│           ├── zh-CN/                  #     简体中文
│           ├── en-US/                  #     英文
│           ├── ja-JP/                  #     日文
│           └── ...                     #     其他35种语言
│
└── idf_component.yml                   # ESP-IDF组件依赖声明
```

---

## 十二、外部依赖组件清单

| 类别 | 组件 | 版本 |
|------|------|------|
| **显示驱动** | esp_lcd_ili9341, esp_lcd_gc9a01, esp_lcd_st7789, esp_lcd_st7796 等 | 1.x - 2.x |
| **显示框架** | lvgl/lvgl | 9.5.0 |
| **LVGL端口** | esp_lvgl_port | 2.7.2 |
| **触控驱动** | esp_lcd_touch_ft5x06, esp_lcd_touch_gt911, esp_lcd_touch_gt1151 等 | ~1.x |
| **音频** | espressif/esp_audio_codec | 2.4.1 |
| **音频效果** | espressif/esp_audio_effects | 1.2.1 |
| **音频设备** | espressif/esp_codec_dev | 1.5.6 |
| **语音识别** | espressif/esp-sr (AFE/WakeNet/MultiNet) | 2.3.0 |
| **WiFi** | 78/esp-wifi-connect | 3.1.3 |
| **4G模块** | 78/esp-ml307 | 3.6.5 |
| **LED** | espressif/led_strip | 3.0.2 |
| **按键** | espressif/button | 4.1.5 |
| **旋钮** | espressif/knob | 1.0.0 |
| **IO扩展** | esp_io_expander_tca9554, esp_io_expander_tca95xx_16bit | 2.0.0 |
| **摄像头** | espressif/esp32-camera, espressif/esp_video | 2.1.6 / 1.3.1 |
| **图片** | espressif/esp_new_jpeg, espressif/esp_image_effects | 0.6.1 / 1.0.1 |
| **字体** | 78/xiaozhi-fonts | 1.6.0 |
| **资源** | espressif/esp_mmap_assets | 1.3.2 |
| **电池** | espressif/adc_battery_estimation | 0.2.1 |
| **表情** | espressif2022/esp_emote_expression, txp666/otto-emoji-gif-component | 0.1.0 / 1.1.1 |
| **IDF版本** | ESP-IDF | >= 5.5.2 |

---

## 十三、项目规模统计

| 维度 | 数量 |
|------|------|
| 支持的板卡 | **~90+** 种 |
| 支持的芯片架构 | **6** 种 (ESP32/S3/C3/C5/C6/P4) |
| 支持的音频编解码芯片 | **6** 种 (ES8311/ES8374/ES8388/ES8389/ES7210/无芯片) |
| 音频编解码器实现类 | **7** 个 |
| 显示类型 | **3** 种 (LCD/OLED/Emote) |
| 通信协议 | **2** 种 (MQTT+UDP / WebSocket) |
| 网络连接方式 | **4** 种 (WiFi/4G/双网/USB-RNDIS) |
| 设备状态 | **11** 个 |
| 主事件类型 | **13** 种 |
| 支持语言 | **38** 种 |
| MCP内置工具 | **11** 个 (6通用 + 5用户) |
| FreeRTOS音频任务 | **3** 个 (输入/输出/编解码) |
| LED实现类 | **3** 种 (Single/Strip/GPIO) |
| 唤醒词实现 | **3** 种 (AFE/Custom/ESP) |
| 音频处理器实现 | **3** 种 (AFE/None/Debugger) |
| IO扩展芯片 | **3** 种 (TCA9554/TCA95xx16/CH32V003) |
| 电池管理芯片 | **2** 种 (AXP2101/SY6970) |

---

## 十四、引脚配置汇总

### 14.1 ESP-BOX-3 完整引脚接线表

| 外设 | 信号 | GPIO | 说明 |
|---|---|---|---|
| **I2S 音频** | MCLK | GPIO_2 | 主时钟 |
| | WS (LRCK) | GPIO_45 | 左右声道时钟 |
| | BCLK (SCLK) | GPIO_17 | 位时钟 |
| | DIN (SD) | GPIO_16 | 数据输入 (麦克风) |
| | DOUT | GPIO_15 | 数据输出 (扬声器) |
| **I2C 音频Codec** | SDA | GPIO_8 | ES8311 + ES7210 共用 |
| | SCL | GPIO_18 | |
| **音频功放** | PA_PIN | GPIO_46 | 功放使能引脚 |
| **按键** | BOOT | GPIO_0 | 多功能按键 |
| | VOLUME_UP | NC | 未使用 |
| | VOLUME_DOWN | NC | 未使用 |
| **显示屏** | 分辨率 | 320x240 | SPI 接口 (内置) |
| | 镜像 | X=true, Y=true | |
| | 背光 | GPIO_47 | |
| **LED** | BUILTIN_LED | NC | 无内置LED |

### 14.2 常见 I2C 引脚组合

#### ESP32-S3 平台

| 组合 | SDA | SCL | 代表板卡 |
|---|---|---|---|
| **S3-8/18** | GPIO_8 | GPIO_18 | ESP-BOX, ESP-BOX-3, ESP-BOX-Lite, ESP-S3-LCD-EV v1.4 |
| **S3-47/48** | GPIO_47 | GPIO_48 | ESP-S3-LCD-EV v1.5/v2, DF-K10, 微雪S3 AMOLED/LCD系列 |
| **S3-38/39** | GPIO_38 | GPIO_39 | M5Stack AtomS3/AtomS3R Echo系列 |
| **S3-41/42** | GPIO_41 | GPIO_42 | 安信可DNESP32S3, XingZhi系列 |
| **S3-1/2** | GPIO_1 | GPIO_2 | 立创开发板, 九川S3, ZhengChen CAM |
| **S3-4/5** | GPIO_4 | GPIO_5 | 安信可DNESP32S3M系列, ESP-SparkBot |
| **S3-15/14** | GPIO_15 | GPIO_14 | 微雪S3 AMOLED/LCD中等尺寸系列 |
| **S3-42/41** | GPIO_42 | GPIO_41 | Kevin-Box-2, Tudouzi, 微雪S3 LCD 0.85 |
| **S3-12/11** | GPIO_12 | GPIO_11 | M5Stack CoreS3 |
| **S3-5/4** | GPIO_5 | GPIO_4 | AiPi-Lite, Movcall-Moji-S3 |
| **S3-44/43** | GPIO_44 | GPIO_43 | Labplus系列 |

#### ESP32-C3 平台

| 组合 | SDA | SCL | 代表板卡 |
|---|---|---|---|
| **C3-0/1** | GPIO_0 | GPIO_1 | 立创C3, 凯文C3, Surfer-C3, Xmini-C3-V3 |
| **C3-3/4** | GPIO_3 | GPIO_4 | Magiclick-C3, Xmini-C3 |
| **C3-21/20** | GPIO_21 | GPIO_20 | Xmini-C3-4G |

#### ESP32-C6 平台

| 组合 | SDA | SCL | 代表板卡 |
|---|---|---|---|
| **C6-8/7** | GPIO_8 | GPIO_7 | 微雪C6 AMOLED/LCD全系列 |

#### ESP32-P4 平台

| 组合 | SDA | SCL | 代表板卡 |
|---|---|---|---|
| **P4-7/8** | GPIO_7 | GPIO_8 | 微雪P4 Nano, P4 WiFi6 Touch LCD系列 |

### 14.3 常见 I2S 引脚配置模式 (Duplex)

| 模式 | MCLK | WS | BCLK | DIN | DOUT | 板卡数量 |
|---|---|---|---|---|---|---|
| **面包板DIY** | NC | 4 | 5 | 6 | 7 | 8+ |
| **乐鑫BOX系列** | 2 | 45 | 17 | 16 | 15 | 2 |
| **ESP-S3-LCD-EV** | 5 | 7 | 16 | 15 | 6 | 2 |
| **安信可S3M** | 6 | 16 | 7 | 17 | 15 | 2 |
| **安信可BOX2** | 38 | 42 | 40 | 39 | 41 | 2 |
| **立创/九川** | 38 | 13 | 14 | 12 | 45 | 2 |
| **M5 Atom Echo** | NC | 6 | 8 | 7 | 5 | 4 |
| **ESP-Korvo2 V3** | 16 | 45 | 9 | 10 | 8 | 2 |
| **微雪S3 3.5寸** | 12 | 15 | 13 | 14 | 16 | 1 |
| **Magiclick** | 8 | 11 | 9 | 10 | 12 | 2 |
| **Labplus** | 39 | 42 | 41 | 40 | 38 | 2 |
| **M5Stack CoreS3** | 0 | 33 | 34 | 14 | 13 | 1 |
| **M5Stack Tab5** | 30 | 29 | 27 | 28 | 26 | 1 |
| **立创C3** | 10 | 12 | 8 | 7 | 11 | 1 |

### 14.4 DIY 面包板推荐引脚 (bread-compact-esp32)

**Duplex 方案（INMP441 + MAX98357）：**

| 功能 | 信号 | GPIO |
|---|---|---|
| I2S 音频 | WS | 4 |
| | BCLK | 5 |
| | DIN | 6 |
| | DOUT | 7 |
| I2C OLED | SDA | 4 |
| | SCL | 15 |
| 按键 | BOOT | 0 |
| | TOUCH | 5 |
| | ASR | 19 |
| LED | GPIO | 2 |

**Simplex 方案（独立麦克风+扬声器）：**

| 功能 | 信号 | GPIO |
|---|---|---|
| 麦克风 I2S | WS / SCK / DIN | 25 / 26 / 32 |
| 扬声器 I2S | DOUT / BCLK / LRCK | 33 / 14 / 27 |

### 14.5 项目中最常用的关键引脚排行

| 引脚 | 用途 | 使用频次 |
|---|---|---|
| **GPIO_0** | BOOT按键 | 60+ 板卡 (ESP32/S3 默认) |
| **GPIO_46** | PA功放 / 背光 | 27+ 板卡 |
| **GPIO_9** | BOOT按键 (C3/C6) | 10+ 板卡 |
| **GPIO_47** | I2C SDA / 背光 | 20+ 板卡 |
| **GPIO_48** | I2C SCL | 15+ 板卡 |
| **GPIO_8** | I2C SDA | 15+ 板卡 (乐鑫系) |
| **GPIO_18** | I2C SCL | 10+ 板卡 (乐鑫系) |
| **GPIO_13** | 背光 (XingZhi/DIY系) | 10+ 板卡 |
| **GPIO_42** | 背光 / I2C SCL | 18+ 板卡 |

### 14.6 引脚设计注意事项

1. **I2C 引脚**: ESP32-S3 最常用 GPIO_47/48 或 GPIO_8/18；ESP32-C3 最常用 GPIO_0/1 或 GPIO_3/4。避开 flash/PSRAM 引脚
2. **I2S 引脚**: Duplex 模式下 MCLK 可选（NC 或独立）。面包板DIY 推荐 WS=4, BCLK=5, DIN=6, DOUT=7
3. **BOOT 引脚**: 必须接芯片 strap pin：ESP32/S3=GPIO_0, C3/C6=GPIO_9, P4=GPIO_35
4. **背光**: ESP32-S3 优先 GPIO_46（最多板卡验证），ESP32-C3 优先 GPIO_2 或 GPIO_13
5. **PA_PIN**: ESP32-S3 用 GPIO_46，若无需功放开关控制可设为 NC
