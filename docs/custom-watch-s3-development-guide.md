# Custom Watch S3 二次开发技术总结

> 作者: dong-dong123  
> 日期: 2026-06-02 ~ 2026-06-05  
> 基础项目: xiaozhi-esp32 (小智语音助手)  
> 硬件: ESP32-S3 + CO5300 AMOLED 410×502 + BMM150 + QMI8658 + NS4168 + MS4030

---

## 一、项目概览

在开源项目 xiaozhi-esp32（小智语音助手）基础上，为 Custom Watch S3 手表 PCB 进行了二次开发。主要工作包括：

1. **新 PCB 引脚适配** — 全部硬件引脚重新映射
2. **音频编解码器重写** — 双独立 I2S 通道的立体声适配
3. **传感器驱动修复与算法优化** — BMM150 内存下溢修复、QMI8658 计步/摇一摇算法
4. **三页滑动手表界面** — 主页、步数、指南针
5. **设置面板** — 主题切换、音量调节、唤醒词选择
6. **天气获取** — HTTP 客户端 + 超时重试
7. **分区表改造** — 扩大 assets 分区支持在线资源烧录

---

## 二、文件结构

```
main/boards/custom-watch-s3/
├── config.h                  # 全部硬件引脚定义
├── custom_watch_s3.cc         # 板级初始化、音频编解码器、传感器管理、天气获取
├── watch_face.h               # 手表界面类声明
├── watch_face.cc              # 手表界面实现（3页滑动 + 设置面板）

main/boards/common/
├── bmm150.cc                  # BMM150 地磁传感器驱动（修复 memset 下溢）
├── bmm150.h
├── qmi8658.cc                 # QMI8658 六轴 IMU（计步 + 摇一摇算法）
├── qmi8658.h

main/audio/
└── audio_service.cc           # 唤醒词 NVS 过滤（已移除）

partitions/v2/
└── 16m_watch.csv              # 自定义分区表

docs/
├── custom-watch-s3-issue-summary.md     # 问题修复总结
├── settings-panel-design.md             # 设置面板设计文档
└── custom-watch-s3-development-guide.md # 本文件
```

---

## 三、硬件引脚分配

`config.h` 定义了所有 GPIO 映射：

| 模块 | 信号 | GPIO | 说明 |
|------|------|------|------|
| **显示屏 (QSPI)** | CS/CLK/SIO0-3/RST/TE | 3/8/15/16/17/18/9/10 | CO5300 AMOLED 410×502 |
| **扬声器 (I2S0 TX)** | BCLK/WS/DOUT | 12/13/11 | NS4168 功放 |
| **麦克风 (I2S1 RX)** | BCLK/WS/DIN | 40/41/39 | MS4030 硅麦 |
| **触摸 (I2C0)** | SCL/SDA/RST/INT | 4/5/7/6 | CST82x 电容触摸 |
| **BMM150 (I2C0)** | 地址 0x13 | 同 I2C0 | 地磁传感器/指南针 |
| **QMI8658 (I2C0)** | 地址 0x6B, INT1 | 同 I2C0, 21 | 六轴 IMU/计步/摇一摇 |
| **RTC (I2C0)** | 地址 0x51, INT | 同 I2C0, 42 | 时钟芯片 |
| **充电管理** | CHECK/STARTUP/ADC | 1/2/14 | 充电状态检测 |
| **按键** | BOOT/VOL+/VOL- | 0/47/48 | 语音助手/音量 |

I2C0 总线上共有 4 个设备：0x13(BMM150)、0x15(CST82x)、0x51(RTC)、0x6B(QMI8658)。

---

## 四、自定义音频编解码器

**背景**: 面包板使用 I2S0 双工模式（TX+RX 共享 BCLK/WS），新 PCB 使用 I2S0 TX + I2S1 RX 独立通道。`NoAudioCodec` 基类的 32-bit 单声道读写逻辑与 16-bit 立体声 I2S 不兼容。

**解决方案**: 实现 `CustomWatchAudioCodec` 类，继承 `NoAudioCodec`，重写 Read/Write 方法。

### 4.1 Read（麦克风读取）

```cpp
int Read(int16_t* dest, int samples) override {
    // I2S1 RX 每个 32-bit FIFO word = 左声道[15:0] + 右声道[31:16]
    // MS4030 的 L/R=GND，数据在左声道
    dest[i] = (int16_t)(buf[i] & 0xFFFF);
}
```

### 4.2 Write（扬声器写入）

```cpp
int Write(const int16_t* data, int samples) override {
    // I2S0 TX，将相同数据填入左右两个声道
    int16_t audio = (volume_adjusted);
    buf[i] = ((int32_t)audio << 16) | ((int32_t)audio & 0xFFFF);
}
```

### 4.3 初始化

```
I2S0 → TX → NS4168 (BCLK=12, WS=13, DOUT=11)
I2S1 → RX → MS4030  (BCLK=40, WS=41, DIN=39)
```

---

## 五、传感器驱动修复

### 5.1 BMM150 内存下溢

**问题**: `bmm150.cc` 构造函数的 `memset` 使用了错误的 offset 计算：
```cpp
// 错误: offsetof(bus_) - offsetof(dig_x1_) = 0 - 12 = -12 → 0xFFFFFFF4 (4GB!)
memset(&dig_x1_, 0, offsetof(bus_) - offsetof(dig_x1_));

// 正确: 仅清零 dig_x1_ ~ dig_xy2_
memset(&dig_x1_, 0, offsetof(dig_xy2_) - offsetof(dig_x1_) + sizeof(dig_xy2_));
```

**影响**: 4GB 的内存写入导致随机崩溃（StoreProhibited、Cache error、InstrFetch）。

### 5.2 BMM150 初始化增强

在读取芯片 ID 之前加入软复位序列：
```
POWER_CTRL = 0x83  (软复位 → bit7=1)
等待 10ms
POWER_CTRL = 0x01  (上电 → 退出 Suspend 模式)
等待 10ms
读取 CHIP_ID (0x40, 期望 0x32)，最多重试 3 次
```

### 5.3 QMI8658 摇一摇唤醒算法

**设计目标**: 响应快速摇晃，屏蔽慢速走路/摆臂动作。

**算法**: 峰值计数法 — 检测加速度幅值穿越阈值形成的完整周期。

```
参数：
  上阈值: 13.0g
  下阈值: 10.0g
  周期数: ≥5 次（1 秒内）
  冷却时间: 5 秒
  数据过滤: 忽略 |acc| > 16.0g 的饱和值

状态机：
  IDLE → (acc > 13.0g) → HIGH
  HIGH → (acc < 10.0g) → HAD_DROP
  HAD_DROP → (acc > 13.0g) → 完成一个周期，记录时间戳
  1秒内 ≥5 个周期 + 距上次触发 > 5秒 → 触发摇一摇
```

### 5.4 QMI8658 计步算法

```
参数：
  触发阈值: 13.0g（需要明显的步伐冲击）
  释放阈值: 11.0g
  防抖间隔: 500ms（两次计步之间的最小间隔）
  数据过滤: 忽略 |acc| > 16.0g 的传感器饱和值
```

### 5.5 抬腕亮屏

```
acc_z > 7.0g 且之前 < 7.0g → 触发抬腕
acc_z < 2.0g → 重置状态
```

---

## 六、三页滑动手表界面

### 6.1 架构

```
container_ (410×502)
├── scroll_container_ (内容宽 1230px, 水平滚动)
│   ├── page_[0] (x=0)    — 首页
│   ├── page_[1] (x=410)  — 步数页
│   └── page_[2] (x=820)  — 指南针页
├── page_dot_[0..2] (overlay) — 页面指示器
├── status_bar_bg_ + wifi + battery + gear (overlay)
└── settings_overlay_ + settings_panel_ (overlay)
```

### 6.2 滚动机制

- `LV_SCROLL_SNAP_START`: 松手吸附到最近的页面
- `LV_OBJ_FLAG_SCROLL_ELASTIC`: 边界弹性效果
- `LV_OBJ_FLAG_SCROLL_CHAIN_HOR` 已移除: 防止边界处滚动传递给父容器
- `LV_EVENT_SCROLL_END` 回调跟踪当前页码，更新指示器

### 6.3 页面 0：首页

```
┌────────────────────┐
│ WiFi  🔋 100%  ⚙  │ ← 状态栏 (34px)
├────────────────────┤
│                    │
│      18:42        │ ← 时钟 (30px × 1.6x scale = 48px)
│                    │
│   6月5日 周三      │ ← 日期 (30px 橙色)
│                    │
│   ☀  多云  30°C  │ ← 天气 (FA图标 + 文字)
│                    │
│    唤醒语音聊天    │ ← 底部点击区域
└────────────────────┘
```

- 时钟: `lv_label` + `transform_scale_x/y(410)` = 1.6x 放大
- pivot 设置为 `lv_pct(50)` 保证居中缩放

### 6.4 页面 1：步数页

```
┌────────────────────┐
│     今日步数        │ ← 标题
│    ╭──弧形──╮      │
│    │  8,420  │      │ ← lv_arc 210×210
│    │   步    │      │   270° 弧形 (135°→405°)
│    ╰───────╯      │   前景: 橙色, 背景: 深灰
│    目标 10000 步   │
└────────────────────┘
```

- 步数更新通过 LVGL timer 每 2 秒从 `step_count_` 读取
- IMU 回调只存储数值，不直接调用 LVGL（避免 task_wdt）

### 6.5 页面 2：指南针页

```
┌────────────────────┐
│      指南针         │
│     ╭ 双环 ╮       │
│       N            │ ← 方向标签 (N=橙色, S/W/E=灰色)
│    W  ▲  E        │
│       │            │ ← 北向菱形指针 (白色, 填充)
│       S            │
│     ╰─────╯       │
│    西南  225°      │ ← 方位角文字 (实时更新)
└────────────────────┘
```

- 16 条刻度线 (lv_line): 4 主(N/S/W/E) + 4 间 + 8 次
- 双环: 外圈 190×190 (3px 边框) + 内圈 172×172 (1px)
- 指针: 白色菱形 (北) + 橙色菱形 (南)，使用 `transform_rotation` 旋转
- BMM150 每 200ms 读取方位角，更新指针和文字

---

## 七、设置面板

### 7.1 入口

状态栏右侧齿轮图标 (FA `gear`, 30px) → 点击弹出面板。

### 7.2 面板结构

```
┌────────────────────────────┐
│  ← 返回         设置       │
├────────────────────────────┤
│  主题     [暗色] [亮色]    │ ← 二选一按钮
│  音量     85%              │
│  ────●────────────────     │ ← lv_slider (0-100)
│  唤醒词                    │
│  [▼ 你好小智          ]    │ ← 点击展开 5 选项
│          你好 大兄弟        │
│          你好 姐妹          │
│          嘿小智             │
│          小智小智           │
│  语音模型                   │
│  云端默认                   │
│  唤醒词需后台同步修改后生效  │ ← 红色提示
├────────────────────────────┤
│       [重启设备]            │ ← 小红按钮
│        [关闭]              │ ← 大橙色按钮
└────────────────────────────┘
```

### 7.3 功能实现

| 功能 | 存储 | 生效方式 |
|------|------|----------|
| 主题 | `Settings("display").SetString("theme")` | 调用 `display->SetTheme()` 立即生效 |
| 音量 | `Settings("audio").SetInt("output_volume")` | 调用 `AudioCodec::SetOutputVolume()` 实时生效 |
| 唤醒词 | `Settings("audio").SetString("wake_word")` | 存 NVS，需后台同步 srmodels.bin |
| 重启 | — | `Application::Reboot()` → `esp_restart()` |

### 7.4 物理按键

- VOL+ (GPIO 47): 单击 +5%，长按 +10%
- VOL- (GPIO 48): 单击 -5%，长按 -10%
- BOOT (GPIO 0): 单击 = 语音聊天开关，长按 = 重启
- 音量调节时屏幕中央弹出 `🔊 85%` 提示，1.5 秒后消失

---

## 八、天气获取

### 8.1 数据源

```
URL: http://wttr.in/Shenzhen?format=j1
超时: 20 秒
重试: 失败立即重试（无限循环直到成功）
刷新: 首次 15 秒后获取，之后每 30 分钟
```

### 8.2 流程

```
StartWeatherTimer (lv_timer, 15s延迟)
  → StartWeatherFetch (FreeRTOS task, stack 8KB)
    → HTTP GET wttr.in
      ├─ 成功 → 解析 JSON → 中文翻译 → lv_async_call(UI更新)
      └─ 失败 → 立即重试 (while true 循环)
```

### 8.3 中文翻译

大小写不敏感的天气描述映射：
- `thunder` → 雷雨
- `rain/drizzle/shower/patchy` → 雨
- `snow/ice` → 雪
- `overcast` → 阴
- `cloud/partly` → 多云
- `sunny/clear` → 晴
- `mist/fog/haze` → 雾

### 8.4 天气图标（FontAwesome）

- `cloud_bolt` (雷雨) / `cloud_rain` (雨) / `snowflake` (雪)
- `smog` (雾) / `clouds` (阴) / `cloud_sun` (多云) / `sun` (晴)

---

## 九、分区表

`partitions/v2/16m_watch.csv`:

```
名称      类型    偏移      大小      说明
nvs       data    0x9000    0x4000    配置存储
otadata   data    0xD000    0x2000    OTA 数据
phy_init  data    0xF000    0x1000    PHY 初始化
ota_0     app     0x20000   0x720000  固件 (7.125MB)
assets    data    0x740000  0x8C0000  资源 (8.75MB)
```

注意: 移除了 `ota_1` 分区以扩大 `assets` 分区容量。因此**不支持 OTA 在线升级固件**，只能 USB 有线刷机。

---

## 十、BUG 修复清单

| # | 问题 | 根因 | 修复 | 文件 |
|---|------|------|------|------|
| 1 | 随机崩溃 | BMM150 memset 下溢 (4GB) | 修正 offset 计算 | bmm150.cc |
| 2 | 麦克风静音 | 立体声数据提取错误 | 重写 Read: `buf[i] & 0xFFFF` | custom_watch_s3.cc |
| 3 | 扬声器杂音 | 右声道收到随机值 | 重写 Write: 左右相同数据 | custom_watch_s3.cc |
| 4 | task_wdt 崩溃 | IMU 回调调用 LVGL 函数 | LVGL timer 轮询步数 | custom_watch_s3.cc |
| 5 | 计步过灵敏 | 阈值太低 (10.8g) | 调至 13.0g / 500ms 防抖 | qmi8658.cc |
| 6 | 摇一摇误触发 | 方差法不准确 | 峰值计数法 (13g/5次/5s) | qmi8658.cc |
| 7 | 天气更新崩溃 | main 任务调用 lv_label_set_text | lv_async_call 转到 LVGL 任务 | custom_watch_s3.cc |
| 8 | 指南针指针不显示 | lv_line 动态更新不渲染 | 改用 lv_obj + transform_rotation | watch_face.cc |
| 9 | 最后一页滑出 | scroll chaining 传递到父容器 | 移除 LV_OBJ_FLAG_SCROLL_CHAIN_HOR | watch_face.cc |
| 10 | 下拉框闪退 | 面板裁剪/scroll拦截 | 切换为按钮组展开收起 | watch_face.cc |
| 11 | 天气显示英文 | strstr 大小写敏感 | 转小写再匹配 | watch_face.cc |
| 12 | 主题亮色白字不可见 | 颜色硬编码 | ApplyTheme() 批量更新所有元素颜色 | watch_face.cc |

---

## 十一、关键技术决策

1. **放弃 lv_line 动态更新**: CO5300 硬件上 lv_line 创建后改点不渲染，改用 `lv_obj` 矩形 + `transform_rotation` 旋转指针

2. **放弃 lv_dropdown**: 下拉列表被面板裁剪且闪退，改用按钮组点击展开/收起

3. **放弃弹性滚动循环**: 页面循环滑动实现复杂且 bug 多，最终保留简单三页滑动

4. **天气 UI 更新跨任务**: 使用 `lv_async_call` 将天气更新从 FreeRTOS 任务桥接到 LVGL 任务

5. **传感器单任务**: BMM150 和 QMI8658 在同一个 FreeRTOS 任务中顺序初始化，避免多个任务抢占 I2C

---

## 十二、数据流图

```
┌─────────────────────────────────────────────────────────────────┐
│                         系统启动                                 │
│  InitializePower → QSPI → Display → Touch → Buttons            │
│  → AudioCodec(Custom I2S) → WatchFace(3 pages + Settings)      │
│  → Weather(lv_timer 15s) → Sensors(I2C: BMM150 + QMI8658)      │
└─────────────────────────────────────────────────────────────────┘

┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│   QMI8658    │    │   BMM150     │    │   天气 HTTP   │
│  IMU Task    │    │  LVGL Timer  │    │  FreeRTOS     │
│  (FreeRTOS)  │    │  (200ms)     │    │  Task         │
└──────┬───────┘    └──────┬───────┘    └──────┬────────┘
       │                   │                   │
       │ step_count_       │ heading           │ desc, temp_c
       │ on_shake          │                   │
       ▼                   ▼                   ▼
┌──────────────────────────────────────────────────────────────┐
│                    CustomWatchS3Board                        │
│  OnImuStep()  OnImuShake()  UpdateCompass()  OnWeatherUpdate()│
└──────────────────────────────────────────────────────────────┘
       │                   │                   │
       ▼                   ▼                   ▼
┌──────────────────────────────────────────────────────────────┐
│                       WatchFace                              │
│  UpdateSteps()  UpdateCompass()  UpdateWeather()             │
│  (LVGL Timer 2s) (LVGL Timer 200ms) (lv_async_call)          │
└──────────────────────────────────────────────────────────────┘
```
