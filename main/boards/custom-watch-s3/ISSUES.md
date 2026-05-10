# CustomWatchS3 问题总结

## 已解决

### 1. 全中文版字体编译后固件过大

**现象**: 使用 `font_puhui_20_4` 等全中文版字体，固件 7.4MB 超过 OTA 分区 4MB

**修复**: 
- 新建 `partitions/v2/16m_watch.csv`：ota_0/ota_1 各扩至 7.2MB，assets 缩至 1.6MB
- sdkconfig 指向新分区表
- 策略：LcdDisplay 用 `font_puhui_basic_*`（聊天 UI 够用），WatchFace 用 `font_puhui_*`（全中文）

### 2. LVGL Canvas 指南针不渲染

**现象**: `lv_canvas` 在此 CO5300 + QSPI 硬件上完全不显示

**修复**: 重写指南针为纯 LVGL 对象组合（`lv_obj` 圆环 + `lv_line` 指针 + `lv_label` 方向标记）

### 3. Chat 消息字体不显示中文

**根因**: `font_puhui_basic_*` 不含中文字形，中文字符渲染为空

**修复**: WatchFace 独立 `LV_FONT_DECLARE(font_puhui_30_4)` 等全中文版字体

### 4. 聊天时文字闪烁/颜色乱码残留

**根因**: `lv_draw_sw_rgb565_swap(px, w*h)` 原地修改了 LVGL 渲染缓存。`LV_DISPLAY_RENDER_MODE_PARTIAL` 模式下 LVGL 复用缓存，非脏区域像素数据已被字节交换污染。

**修复**: `custom_watch_s3.cc` flush 回调改为先 `memcpy` 到独立 DMA 缓存再 swap，LVGL 原始缓存保持干净。

### 5. 聊天退下后无法切回手表界面

**根因**: 聊天结束后 `blink_timer` 残留的 LcdDisplay 表情遮挡了 WatchFace。后来还有一个根因是 CO5300 休眠（问题 7）。

**修复**: 
- 移除 idle 时的 `SetEmotion()` 调用
- 使用 `show_watch` 条件（idle/starting/configuring/activating）完整覆盖

### 6. 聊天中 CO5300 息屏

**根因**: 表盘模式有时钟每 1 秒刷新自然产生 SPI 通信，但聊天模式表盘隐藏 + 无持续刷新 → CO5300 APS 硬件定时器超时自睡。

**修复**: `lv_timer` 每 3 秒 `lv_obj_invalidate(lv_scr_act())`——聊天和表盘模式下都触发 LVGL 渲染产生像素数据传输。

### 7. 天气不显示

**根因**:
- 最早 `StartWeatherTimer()` 因调试崩溃被注释
- 启用后 `http->ReadAll()` 无限阻塞（wttr.in 纯 HTTP 连接不发数据）
- `esp_http_client` 10 秒超时不够（wttr.in 响应 39KB 需 20+ 秒）
- 温度 `valuedouble` 解析字符串失败显示 0°C

**修复**:
- 改用 `esp_http_client` API，30 秒超时，跳过证书验证
- HTTP 在独立 FreeRTOS 任务执行，不阻塞应用调度器
- 温度用 `atoi(valuestring)` 解析
- 天气描述翻译为中文（Partly cloudy→多云 等）
- FontAwesome 天气图标映射修复
- CMakeLists.txt 添加 `esp_http_client` 依赖

### 8. QMI8658 寄存器映射错误

**根因**: 对照官方 Arduino QMI8658C 参考驱动，发现寄存器地址全部错位：
- 软复位写到加速度配置寄存器（0x03 而非 0x60）
- 传感器使能写到陀螺仪配置寄存器（0x04 而非 0x08）
- 传感器从未通电（CTRL7 bit[1:0]=00）

**修复**: 对照数据手册重写 Init()，加入 REG_RESET (0x60)，正确 CTRL2/CTRL3/CTRL7 地址

### 9. QMI8658C WHO_AM_I=0x30 不识别

**根因**: QMI8658C 变体的 WHO_AM_I 是 0x30（QMI8658A 是 0x05）

**修复**: `Init()` 中同时接受 0x05 和 0x30

### 10. QMI8658C I2C 寄存器读写失败（部分解决）

**现象**: WHO_AM_I (0x00) 能读到 0x30，但所有其他寄存器读回都是 0x30。I2C 内部寄存器指针不更新。

**尝试过**:
- `i2c_master_transmit_receive`（repeated START）→ 返回 0x30
- 分步 `transmit` + `receive`（中间 STOP）→ 返回 0x30  
- I2C 速度 400kHz→100kHz
- 地址 0x6A↔0x6B

**当前状态**: 芯片在 I2C 上应答但寄存器写不生效，需要逻辑分析仪看总线波形进一步定位。QMI8658 Init 会失败但不阻塞启动。

---

## 待处理

| 问题 | 优先级 | 说明 |
|------|--------|------|
| QMI8658 计步 | 中 | I2C 寄存器问题解决后才能工作 |
| BMM150 指南针 | 低 | 待获取数据手册后调试 |
| 天气图标美化 | 低 | 当前 FontAwesome 图标可接受 |
