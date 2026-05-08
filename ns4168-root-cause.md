# NS4168 喇叭无声问题根因分析

## 现象

固件正常运行、WiFi 连接成功、语音对话交互正常（服务器能识别语音并返回 TTS 回复），但 NS4168 功放模块连接的喇叭完全无声。

## 排查过程

### 第一步：排除硬件故障

- ESP32-S3 板卡正常（同一块板用 MAX98357A + INMP441 配置可正常工作）
- NS4168 供电 5V 正常
- 喇叭正常（物理连接无误）
- Pin 3 "SDA" 尝试接 3.3V / GND / GPIO14 均无效

### 第二步：排除接线错误

尝试了多种接线组合：
- GPIO 14 → Pin 3 "SDA" → 无声
- GPIO 14 → Pin 4 "DATA" → 无声
- GPIO 7 提供 MCLK 6.144MHz → 无声
- Pin 5 "CLK" 接或不接 MCLK → 均无声

排除接线问题。

### 第三步：对比参考实现

在 `D:\Download\xiaozhi-esp32\xiaozhi-esp32` 中找到安信可 `atk-dnesp32s3-box` 板卡，该板卡同样使用 NS4168 功放，且工作正常。

## 根因

**I2S 协议格式不匹配。**

安信可板卡为 NS4168 专门实现了一个 `ATK_NoAudioCodecDuplex` 类，其 I2S 参数与项目默认的 `NoAudioCodecDuplex` 完全不同：

| 参数 | NoAudioCodecDuplex（默认） | ATK_NoAudioCodecDuplex（NS4168） |
|------|--------------------------|--------------------------------|
| `data_bit_width` | `I2S_DATA_BIT_WIDTH_32BIT` | **`I2S_DATA_BIT_WIDTH_16BIT`** |
| `slot_mode` | `I2S_SLOT_MODE_MONO` | **`I2S_SLOT_MODE_STEREO`** |
| `slot_mask` | `I2S_STD_SLOT_LEFT` | **`I2S_STD_SLOT_BOTH`** |
| `ws_width` | `I2S_DATA_BIT_WIDTH_32BIT` | **`I2S_DATA_BIT_WIDTH_16BIT`** |

NS4168 芯片的 I2S 从机接口只接受 **16-bit 立体声** 格式。当 ESP32 以 32-bit 单声道左对齐格式发送数据时，NS4168 无法正确解析帧结构，DAC 无输出，喇叭完全无声。

**参数含义解释：**

- `data_bit_width = 16BIT`：每声道 16 位有效数据（32BIT 会多出 16 位无效填充，导致 NS4168 误判帧边界）
- `slot_mode = STEREO`：双声道模式（MONO 模式下 WS 信号波形不同，NS4168 不识别）
- `slot_mask = BOTH`：左右声道均输出（LEFT 只输出左声道，不符合 NS4168 预期）
- `ws_width = 16BIT`：WS 脉冲宽度为 16 个 BCLK（32BIT 时 WS 宽 32 BCLK，NS4168 不兼容）

## 解决方案

在 `custom_watch_s3.cc` 中创建 `NS4168AudioCodec` 类，继承 `NoAudioCodec`，I2S 参数完全对齐 ATK 版配置。`GetAudioCodec()` 改用新类。

```cpp
class NS4168AudioCodec : public NoAudioCodec {
    // I2S: 16-bit, STEREO, BOTH slots, 16-bit WS
};
```

## 修改文件

`main/boards/custom-watch-s3/custom_watch_s3.cc`
- 新增 `NS4168AudioCodec` 类
- `GetAudioCodec()` 返回 `NS4168AudioCodec` 实例

---

## 追加问题：修改 I2S 后麦克风无法拾音

### 现象

将 I2S 改为 16-bit 立体声后，喇叭有声了，但麦克风识别不到说话——设备始终停留在 `listening` 状态，不进入 `speaking`。

### 根因

最初的修复将 TX 和 RX 都统一改成 16-bit 立体声：

```cpp
// 错误：TX 和 RX 用同一套配置
ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
```

NS4168 功放（TX）需要 16-bit 立体声，但 MS4030 麦克风（RX）是单声道设备。将 RX 也改为立体声后，MS4030 输出的数据在 I2S 帧中的位置偏移，导致 ESP32 收到的音频数据全是无效值，VAD 检测不到人声。

**正确做法**：TX 和 RX 使用独立的 I2S 配置。

| 通道 | 数据位宽 | 声道模式 | 槽位掩码 | WS 宽度 | 用途 |
|------|---------|---------|---------|--------|------|
| TX | 16BIT | STEREO | BOTH | 16BIT | NS4168 功放输出 |
| RX | 32BIT | MONO | LEFT | 32BIT | MS4030 麦克风输入 |

### 解决方案

```cpp
// TX: 16-bit 立体声 → NS4168
i2s_std_config_t tx_cfg = { ... };
ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_cfg));

// RX: 32-bit 单声道 ← MS4030
i2s_std_config_t rx_cfg = { ... };
ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_cfg));
```

---

## 最终正确接线

| NS4168 Pin | 丝印 | 接 ESP32-S3 |
|-----------|------|------------|
| 1 | LRCKL | GPIO 16 |
| 2 | BCLK | GPIO 15 |
| 3 | SDA | GPIO 14 |
| 4 | DATA | 悬空 |
| 5 | CLK | 悬空 |
| 6 | VCC | 5V |
| 7 | GND | GND |
| — | SPK | 喇叭 2~8Ω |
