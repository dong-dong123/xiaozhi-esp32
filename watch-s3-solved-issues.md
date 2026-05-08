# 小智手表 custom-watch-s3 问题解决记录

## 最终状态

| 功能 | 状态 |
|------|------|
| CO5300 AMOLED 屏幕 | ✅ 正常显示 |
| CST820 触摸 | ✅ 正常（非阻塞初始化） |
| NS4168 功放喇叭 | ✅ 有声（需 5V 供电） |
| MS4030 麦克风 | ✅ 正常录音 |
| WiFi 联网 | ✅ 正常 |
| MQTT 语音对话 | ✅ 正常 |
| 唤醒词检测 | ✅ 降低 VAD 阈值后正常 |
| BOOT 按键 (GPIO0) | ✅ 加了上拉不重启 |

---

## 问题 1：ESP-BOX-3 启动崩溃

**现象**：烧录后 I2C 报错 `Fail to write to dev 30`，assert 崩溃

**根因**：裸板 ESP32-S3 没有 ES8311 音频芯片，固件按 ESP-BOX-3 配置去 I2C 读不存在的芯片

**解决**：创建自定义板卡 `custom-s3-wroom-1`，改用 `NoAudioCodecDuplex`（I2S 直连）

---

## 问题 2：NS4168 喇叭无声

**现象**：WiFi 连接正常、语音对话正常、TTS 有回复，但喇叭完全无声

**排查过程**：
1. 换 Pin 3/4 接线 → 无效
2. 加 MCLK（GPIO7, 6.144MHz）→ 无效
3. 独立测试任务 1kHz 正弦波 → 仍无声
4. 万用表测 NS4168 5V 供电 → **只有 3.3V**

**根因**：旧开发板 USB 5V 输出异常，NS4168 功放供电不足

**解决**：换开发板后 5V 正常，喇叭恢复

---

## 问题 3：MS4030 在 16-bit STEREO 下不工作

**现象**：为适配 NS4168 改 I2S 为 16-bit STEREO 后，麦克风无法拾音，唤醒词失效

**根因**：Duplex 模式下 TX 和 RX 共享 BCLK/WS，16-bit STEREO 的 WS 波形（50% 占空比）与 MS4030 MEMS 麦克风不兼容。MS4030 仅在 32-bit MONO WS 波形下正常输出

**解决**：回退到原始 `NoAudioCodecDuplex`（32-bit MONO），保持麦克风兼容

---

## 问题 4：唤醒词不生效（只能按键对话）

**现象**：在 idle 状态下说"你好小智"无反应，必须按 BOOT 键才能触发对话。对话中麦克风正常（STT 能识别语音）

**根因**：新板子 MS4030 灵敏度与旧板不同，AFE 的 VAD（Voice Activity Detection）灵敏度不够，语音数据在到达 WakeNet 前就被 VAD 过滤掉了

**解决**：`afe_wake_word.cc` 中 `vad_mode = 0`（最低灵敏度），条件编译仅对 `CONFIG_BOARD_TYPE_CUSTOM_WATCH_S3` 生效

---

## 问题 5：BOOT 键按下就重启

**现象**：按 GPIO 0 的 BOOT 按键，ESP32 直接重启进入下载模式

**根因**：GPIO 0 是 ESP32-S3 的 strap 引脚。新板没有外接上拉电阻，按键时电压波动触发芯片误判进下载模式

**解决**：`InitializePower()` 中对 GPIO 0 启用内部上拉（`GPIO_PULLUP_ENABLE`）

---

## 问题 6：CST820 触摸 I2C 初始化卡死

**现象**：`esp_lcd_touch_new_i2c_cst820` 调用时 I2C 通信失败，阻塞主线程触发 Task Watchdog

**根因**：新板子 GPIO4/5 I2C 总线电气特性不稳定，CST820 偶尔不应答

**解决**：去掉 `ESP_ERROR_CHECK`，改为手动检查返回值——失败时打印警告并跳过触摸初始化，系统继续运行（用 BOOT 按键替代触摸）

---

## 问题 7：CO5300 显示初始化时的 SPI 冲突

**现象**：背光设置时 `spi_device_acquire_bus: Cannot acquire bus`

**根因**：LVGL 渲染任务和背光亮度设置（CO5300 0x51 命令）共用同一 SPI 总线，构造时异步冲突

**解决**：
1. 背光 `RestoreBrightness()` 改到 LVGL 定时器延后 300ms 执行
2. `SetBrightnessImpl()` 加 `DisplayLockGuard` 防止后续冲突

---

## 问题 8：WatchFace 创建时 LVGL 空指针崩溃

**现象**：`LoadProhibited` at `lv_obj_style.c`，`PC=0x00000000`

**根因**：板卡构造函数中立即创建 WatchFace 控件，此时 LVGL 尚未完成首帧渲染，`lv_scr_act()` 返回的屏幕样式未初始化

**解决**：WatchFace 创建改为 LVGL 一次性定时器延后 500ms 执行

---

## 问题 9：CO5300 不支持 mirror_y / swap_xy

**现象**：`co5300_spi: mirror_y is not supported` 错误

**根因**：CO5300 驱动不支持 Y 轴镜像和 XY 交换，配了 true

**解决**：config.h 中 `DISPLAY_MIRROR_Y=false`, `DISPLAY_SWAP_XY=false`

---

## 问题 10：LVGL 9.x API 兼容

**现象**：`lv_font_montserrat_48` / `lv_draw_line` / `lv_draw_buf_init` 编译报错

**根因**：项目用 LVGL 9.5.0，API 和旧版不同：
- 内置字体只有 `lv_font_montserrat_14`
- `lv_draw_line` 只需 2 参数（点坐标在 dsc 中）
- `lv_draw_buf_init` 需 7 参数

**解决**：用项目字体 `BUILTIN_TEXT_FONT` + `LV_FONT_DECLARE`，适配 LVGL 9.x API

---

## 问题 11：编译链接错误

| 错误 | 解决 |
|------|------|
| `esp_http_client.h: No such file` | 改用项目 `Http` 类（`<http.h>`, `Open()`/`ReadAll()`/`Close()`） |
| `i2c_master_dev_delete` 未声明 | 改为 `i2c_master_bus_rm_device` |
| `pdMS_TO_TICKS` / `vTaskDelay` 未声明 | bmm150.cc 补 `#include <freertos/FreeRTOS.h>` |
| `font_puhui_basic_20_4` 未声明 | `watch_face.cc` 补 `LV_FONT_DECLARE(BUILTIN_TEXT_FONT)` |
| BMM150/QMI8658 未定义引用 | CMakeLists.txt 补 `.cc` 到 SOURCES 列表 |
| `bmm150.cc`/`qmi8658.cc` 未编译 | 同上 |
