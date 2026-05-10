# CustomWatchS3 问题解决记录

## 已解决问题

### 1. CO5300 显示屏间歇性极暗

**根因**: CO5300 面板自动休眠（APS 硬件定时器），`0x11`(Sleep Out) 仅 init 时发送一次，面板超时后自行休眠无人唤醒。

**修复**: 
- `custom_watch_s3.cc`: 用 `esp_timer` 每 5 秒调用 `esp_lcd_panel_disp_on_off(panel_, true)`，发送 DCS `0x29`(Display On) 命令在硬件寄存器层面保持面板唤醒。
- `esp_timer` 运行在 `ESP_TIMER_TASK` 上下文，优先级独立于 LVGL 任务，语音聊天时也不会被饿死。

### 2. 聊天时文字闪烁/颜色乱码残留

**根因**: LVGL flush 回调中 `lv_draw_sw_rgb565_swap(px, w*h)` 原地修改了 LVGL 渲染缓冲区。`LV_DISPLAY_RENDER_MODE_PARTIAL` 模式下 LVGL 复用缓冲区，非脏区域的像素数据已被字节交换破坏。后续文字抗锯齿混合时读取到错误的背景色 → 闪烁/残留。

**修复**:
- `custom_watch_s3.cc`: Flush 回调改为先 `memcpy` 像素到独立 DMA 缓冲区，在副本上做字节交换后发送。LVGL 原始缓冲区保持干净。

### 3. 聊天退下后无法切回手表界面

**根因**: 问题 1 的延续——CO5300 面板已经休眠，软件层面 `watch_face_->Show()` 正常执行但屏幕不亮。

**修复**: 问题 1 修好后自行解决。

### 4. Chat 消息字体不显示中文 / 日期显示 "510"

**根因**: `font_puhui_basic_*` 是不含中文字符的精简版字体，中文字符渲染为空。

**修复**:
- `CMakeLists.txt`: `BUILTIN_TEXT_FONT` 保持 `font_puhui_basic_20_4`（聊天 UI 够用）
- `watch_face.cc`: 表盘独立 `LV_FONT_DECLARE(font_puhui_30_4)` 等完整中文版字体
- `partitions/v2/16m_watch.csv`: OTA 分区从 4MB 扩至 7.2MB 容纳全中文版字体

### 5. LVGL Canvas 指南针不渲染

**根因**: LVGL Canvas 在此 CO5300 + QSPI 硬件组合上不工作。

**修复**: 重写指南针为纯 LVGL 对象组合（`lv_obj` 圆环 + `lv_line` 指针 + `lv_label` 方向标记）。

---

## 待解决问题

### 6. 天气不显示

`StartWeatherTimer()` 因之前调试崩溃被注释，天气获取代码完整但未启用。需排查 `StartWeatherTimer()` 安全启用时机。

### 7. 步数不显示

步数依赖 QMI8658 IMU 传感器。`InitializeSensors()` 在 WatchFace 创建前调用会导致堆竞争崩溃（TLSF block_is_last 断言）。需排查：
- QMI8658/BMM150 驱动是否正确
- I2C 总线是否有 ACK 响应
- FreeRTOS 任务栈是否足够
- 与 LVGL 堆分配是否存在竞争
