# 自定义板卡配置指南：ESP32-S3-WROOM-1 + INMP441 + MAX98357A + ST7789

> 创建日期：2026-05-02

---

## 一、硬件接线总览

### 1.1 ESP32-S3-WROOM-1 模组

蓝色裸板最小系统板，USB-C 供电/烧录，两侧排针引出 GPIO。

### 1.2 完整接线表

| 外设 | 模块引脚 | 功能 | ESP32-S3 GPIO | 备注 |
|------|---------|------|--------------|------|
| **ST7789 屏幕** | VCC | 电源正 | 3V3 | |
| | GND | 电源地 | GND | |
| | SCL | SPI时钟 | **GPIO 12** | |
| | SDA | SPI数据 | **GPIO 11** | |
| | RES | 复位 | **GPIO 5** | |
| | DC | 数据/命令 | **GPIO 4** | |
| | CS | 片选 | **GPIO 10** | |
| | BLK | 背光 | 3V3 | 常亮（不可调光） |
| **INMP441 麦克风** | VDD | 电源正 | 3V3 | |
| | GND | 电源地 | GND | |
| | SD | 数据输出 | **GPIO 14** | |
| | WS | 帧时钟 | **GPIO 16** | 与扬声器共用 |
| | SCK | 位时钟 | **GPIO 15** | 与扬声器共用 |
| | L/R | 左右声道 | GND | 接GND选择左声道 |
| **MAX98357A 扬声器** | VIN | 电源正 | 5V | 建议5V更大音量 |
| | GND | 电源地 | GND | |
| | DIN | 数据输入 | **GPIO 7** | |
| | BCLK | 位时钟 | **GPIO 15** | 与麦克风共用 |
| | LRC | 帧时钟 | **GPIO 16** | 与麦克风共用 |
| | SD_MODE | 关断控制 | 3V3 | 高电平使能 |
| | GAIN | 增益设置 | GND | 12dB增益 |
| | OUT+ | 喇叭正极 | 喇叭红线 | |
| | OUT- | 喇叭负极 | 喇叭黑线 | |

### 1.3 接线原理图

```
                    ESP32-S3-WROOM-1
                   ┌──────────────────┐
                   │                  │
    ST7789 LCD ─── │ GPIO12 (SCLK)    │
    240x240    ─── │ GPIO11 (MOSI)    │
              ───  │ GPIO10 (CS)      │
              ───  │ GPIO4  (DC)      │
              ───  │ GPIO5  (RST)     │
                   │                  │
    INMP441   ───  │ GPIO14 (I2S DIN) │──────┐
    (麦克风)   ─── │ GPIO16 (I2S WS)  │──┐   │
              ───  │ GPIO15 (I2S BCLK)│─┐│   │
                   │                  │ ││   │
    MAX98357A ───  │ GPIO7  (I2S DOUT)│ ││   │
    (扬声器)   ─── │ GPIO16 (I2S WS)  │─┘│   │ ← 共享WS
              ───  │ GPIO15 (I2S BCLK)│──┘   │ ← 共享BCLK
                   │                  │      │
    BOOT按钮  ───  │ GPIO0            │      │
                   └──────────────────┘      │
                                             │
          I2S 总线拓扑:                       │
          ┌──────────────────────────────────┘
          │  BCLK = GPIO15 (共用)
          │  WS   = GPIO16 (共用)
          │  DIN  = GPIO14 ← INMP441 SD
          │  DOUT = GPIO7  → MAX98357A DIN
          ▼
      同一条 I2S Duplex 总线
```

---

## 二、问题诊断：为什么 ESP-BOX-3 不能用于此硬件

### 2.1 原始崩溃日志

```
I (218) Board: UUID=9b8c1406-3b32-41cd-bb93-08c9fc69a6c9 SKU=esp-box-3
I (218) ili9341: LCD panel create success
...
E (478) I2C_If: Fail to read from dev 30
E (478) I2C_If: Fail to write to dev 30
... (几十条 I2C 失败) ...
E (558) ES8311: Open fail
assert failed: box_audio_codec.cc:51 (out_codec_if_ != NULL)
```

### 2.2 根因分析

| 层级 | ESP-BOX-3 期望 | 实际硬件 | 结果 |
|------|---------------|---------|------|
| 音频 Codec | ES8311 I2C 芯片 (地址 0x30) | 不存在 | I2C 通信超时 |
| 音频 ADC | ES7210 I2C 芯片 | 不存在 | - |
| I2C 总线 | SDA=GPIO8, SCL=GPIO18 | 无设备应答 | 超时 |
| 显示屏 | ILI9341 (SPI) | ST7789 | 初始化失败 |
| 代码路径 | `BoxAudioCodec` | 需要 `NoAudioCodecDuplex` | assert 崩溃 |

**结论：ESP-BOX-3 是乐鑫官方带外壳的完整 AIoT 套件，内置 ES8311+ES7210 音频芯片和 ILI9341 屏幕。裸板 ESP32-S3-WROOM-1 完全没有这些芯片，必须使用自定义板卡配置。**

### 2.3 方案选择

项目中没有任何现成板卡与此接线完全匹配，因为：

1. **I2S 引脚独特**：MIC 使用 GPIO 14/16/15，而所有面包板系列使用 GPIO 6/4/5
2. **SPI 引脚独特**：MOSI=11/SCLK=12 仅 `eda-tv-pro` 匹配，但 DC/CS/RST 全部不同
3. **Duplex 模式**：INMP441 + MAX98357A 共享同一条 I2S 总线（WS/BCLK 共用），属于 Duplex 模式

→ **必须创建自定义板卡**。

---

## 三、新建文件清单

### 3.1 `main/boards/custom-s3-wroom-1/config.json`

```json
{
    "target": "esp32s3",
    "builds": [
        {
            "name": "custom-s3-wroom-1",
            "sdkconfig_append": [
                "LCD_ST7789_240X240=y"
            ]
        }
    ]
}
```

**说明**：
- `target`: 指定芯片为 ESP32-S3
- `name`: 构建目标名称
- `sdkconfig_append`: 默认启用 ST7789 240x240 配置

### 3.2 `main/boards/custom-s3-wroom-1/config.h`

```c
#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// I2S Duplex (INMP441 + MAX98357A 共享 BCLK/WS 总线)
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_16
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_14
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7

// ST7789 SPI 显示屏 (240x240)
#define DISPLAY_MOSI_PIN      GPIO_NUM_11
#define DISPLAY_CLK_PIN       GPIO_NUM_12
#define DISPLAY_DC_PIN        GPIO_NUM_4
#define DISPLAY_RST_PIN       GPIO_NUM_5
#define DISPLAY_CS_PIN        GPIO_NUM_10
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC

#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0

// 按键和LED
#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_
```

**关键配置说明**：

| 宏 | 值 | 原因 |
|---|---|---|
| `AUDIO_INPUT_SAMPLE_RATE` | 16000 | INMP441 标准采样率 |
| `AUDIO_OUTPUT_SAMPLE_RATE` | 24000 | MAX98357A 推荐采样率 |
| `AUDIO_I2S_GPIO_BCLK` | 15 | 麦克风和扬声器共享同一位时钟 |
| `AUDIO_I2S_GPIO_WS` | 16 | 麦克风和扬声器共享同一帧时钟 |
| `AUDIO_I2S_GPIO_DIN` | 14 | INMP441 SD 数据输出 |
| `AUDIO_I2S_GPIO_DOUT` | 7 | MAX98357A DIN 数据输入 |
| `DISPLAY_BACKLIGHT_PIN` | NC | 背光直接接 3V3，不可程序控制 |
| `DISPLAY_SPI_MODE` | 0 | ST7789 标准 SPI 模式 |

### 3.3 `main/boards/custom-s3-wroom-1/custom_s3_wroom1_board.cc`

```cpp
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#define TAG "CustomS3Wroom1Board"

class CustomS3Wroom1Board : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay* display_;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    CustomS3Wroom1Board() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CustomS3Wroom1Board);
```

**代码架构说明**：
- 继承 `WifiBoard`（WiFi 联网能力）
- 使用 `NoAudioCodecDuplex`（无外部 I2C Codec，I2S 直连）
- 使用 `SpiLcdDisplay`（ST7789 SPI 屏）
- 通过 `DECLARE_BOARD` 宏注册为编译时工厂单例

---

## 四、修改的现有文件

### 4.1 `main/Kconfig.projbuild`

**修改位置**：第 132 行之后插入

```diff
     config BOARD_TYPE_BREAD_COMPACT_WIFI_CAM
         bool "Bread Compact WiFi + LCD + Camera (面包板)"
         depends on IDF_TARGET_ESP32S3
+    config BOARD_TYPE_CUSTOM_S3_WROOM_1
+        bool "Custom S3 WROOM-1 (INMP441+MAX98357+ST7789)"
+        depends on IDF_TARGET_ESP32S3
     config BOARD_TYPE_BREAD_COMPACT_ML307
         bool "Bread Compact ML307/EC801E (面包板 4G)"
```

**作用**：在 `menuconfig` 的 `Board Type` 选择菜单中新增一个选项。

### 4.2 `main/CMakeLists.txt`

**修改位置**：第 534 行之后插入

```diff
 elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_LCD)
     set(BOARD_TYPE "bread-compact-wifi-lcd")
     set(BUILTIN_TEXT_FONT font_puhui_basic_16_4)
     set(BUILTIN_ICON_FONT font_awesome_16_4)
     set(DEFAULT_EMOJI_COLLECTION twemoji_32)
+elseif(CONFIG_BOARD_TYPE_CUSTOM_S3_WROOM_1)
+    set(BOARD_TYPE "custom-s3-wroom-1")
+    set(BUILTIN_TEXT_FONT font_puhui_basic_16_4)
+    set(BUILTIN_ICON_FONT font_awesome_16_4)
+    set(DEFAULT_EMOJI_COLLECTION twemoji_32)
 elseif(CONFIG_BOARD_TYPE_TUDOUZI)
     set(BOARD_TYPE "tudouzi")
```

**作用**：
1. 将 `BOARD_TYPE` 映射到 `custom-s3-wroom-1` 目录
2. 设置默认字体（`font_puhui_basic_16_4`）
3. 设置默认图标字体（`font_awesome_16_4`）
4. 设置默认 Emoji 集合（`twemoji_32`）

构建系统通过 `BOARD_TYPE` 变量自动找到 `main/boards/custom-s3-wroom-1/` 目录下的 `.cc` 源文件。

---

## 五、编译与烧录步骤

### 5.1 激活 ESP-IDF 环境

```powershell
# 在 PowerShell 中执行
C:\Espressif\Initialize-Idf.ps1
```

> **注意**：根据之前的排查，ESP-IDF v5.5.4 已安装在 `C:\Espressif\frameworks\esp-idf-v5.5.4`，但 `IDF_PATH` 环境变量未设置。执行上述脚本即可激活。

### 5.2 配置项目

```bash
# 进入项目目录
cd c:\Users\Admin\Desktop\AIxiaozhi\xiaozhi-esp32

# 设置目标芯片为 ESP32-S3
idf.py set-target esp32s3
```

### 5.3 menuconfig 选择板卡

```bash
idf.py menuconfig
```

在菜单界面中：
1. 进入 `Xiaozhi Assistant`
2. 选择 `Board Type`
3. 选择 **`Custom S3 WROOM-1 (INMP441+MAX98357+ST7789)`**
4. 按 `S` 保存，按 `Q` 退出

其他推荐配置：

| 菜单路径 | 选项 | 推荐值 |
|---------|------|-------|
| `Xiaozhi Assistant → Wake Word` | 唤醒词类型 | `Wake Word Disabled` (先关闭，确认硬件正常后再开启) |
| `Xiaozhi Assistant → Audio Processing` | Audio Processor | 不勾选 (INMP441 无参考通道，不支持 AEC) |
| `Xiaozhi Assistant → WiFi Config Method` | 配网方式 | `Hotspot WiFi Provisioning` |

### 5.4 编译

```bash
idf.py build
```

首次编译会下载依赖组件（lvgl、esp_audio_codec、led_strip 等），耗时较长。

### 5.5 烧录

```bash
# 按住 BOOT 键（GPIO 0），然后按一下 EN 键进入下载模式
idf.py flash
```

### 5.6 查看串口日志

```bash
idf.py monitor
```

正常启动后，日志应显示：
```
I (xxx) Board: UUID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx SKU=custom-s3-wroom-1
I (xxx) CustomS3Wroom1Board: Install LCD driver
I (xxx) WifiBoard: Starting WiFi...
```

**不应再出现** `I2C_If: Fail to write to dev 30` 这种错误。

---

## 六、可能遇到的问题及排查

### 6.1 屏幕不亮/显示异常

ST7789 240x240 屏幕的初始化参数可能需要微调。尝试在 `config.h` 中修改以下参数：

```c
// 常见调整项
#define DISPLAY_INVERT_COLOR    false   // 尝试反转
#define DISPLAY_SWAP_XY         true    // 尝试交换XY
#define DISPLAY_MIRROR_X        true    // 尝试X镜像
#define DISPLAY_MIRROR_Y        true    // 尝试Y镜像
#define DISPLAY_SPI_MODE        3       // 某些7针屏幕需要模式3
```

### 6.2 麦克风无声音

1. 检查 INMP441 的 L/R 引脚是否接地
2. 检查 WS 和 SCK 是否与扬声器共享同一 GPIO
3. 确认 `AUDIO_INPUT_SAMPLE_RATE` 为 16000

### 6.3 扬声器无声

1. 检查 MAX98357A 的 VIN 是否接了 5V（不是 3V3）
2. 检查 SD_MODE 是否接了 3V3（高电平使能）
3. 确认 `AUDIO_OUTPUT_SAMPLE_RATE` 为 24000

### 6.4 如果屏幕参数不匹配

你的屏幕可能是 240x320（竖屏）而非 240x240（方屏）。确认后修改 `config.h`：

```c
// 240x320 竖屏配置
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
```

同时在 `config.json` 中将 `LCD_ST7789_240X240=y` 改为 `LCD_ST7789_240X320=y`。

---

## 七、文件清单汇总

```
新增文件 (3个):
├── main/boards/custom-s3-wroom-1/config.json          # 板卡元数据
├── main/boards/custom-s3-wroom-1/config.h             # 引脚定义
└── main/boards/custom-s3-wroom-1/custom_s3_wroom1_board.cc  # 板卡实现

修改文件 (2个):
├── main/Kconfig.projbuild        # +4行：新增 BOARD_TYPE_CUSTOM_S3_WROOM_1
└── main/CMakeLists.txt           # +4行：新增 BOARD_TYPE 映射

参考文件 (3个模板):
├── main/boards/bread-compact-wifi-lcd/config.h        # I2S Duplex+ST7789 参考
├── main/boards/bread-compact-wifi-lcd/compact_wifi_board_lcd.cc  # 板卡实现模板
└── docs/custom-board_zh.md                            # 官方自定义板卡指南
```

---

## 八、架构对比

| 特性 | ESP-BOX-3 (原错误选择) | Custom S3 WROOM-1 (正确选择) |
|------|----------------------|---------------------------|
| 基类 | WifiBoard | WifiBoard |
| 音频 Codec | BoxAudioCodec (ES8311+ES7210) | NoAudioCodecDuplex (直连I2S) |
| I2C 依赖 | 是（ES8311 地址 0x30, ES7210） | 无 |
| I2S 模式 | Duplex + MCLK | Duplex（无 MCLK） |
| 显示屏 | ILI9341 (SPI, 320x240) | ST7789 (SPI, 240x240) |
| 背光 | GPIO47 (PWM可调) | 3V3 常亮 |
| 按键 | BOOT + 双击AEC切换 | BOOT（仅单击） |
| 回声消除 | 支持（输入参考通道） | 不支持（单通道麦） |
| 功放控制 | GPIO46 | 直连 3V3 常开 |
