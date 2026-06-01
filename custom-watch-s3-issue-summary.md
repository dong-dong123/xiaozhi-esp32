# Custom Watch S3 PCB 适配问题总结

> 日期：2026-06-01 ~ 2026-06-02  
> 硬件：ESP32-S3 + CO5300 AMOLED + CST82x Touch + NS4168 + MS4030 + QMI8658 + BMM150

---

## 问题 1：新 PCB 引脚适配

**现象**：新焊接的 PCB 板引脚布局与旧面包板完全不同。

**解决**：更新 `config.h` 和 `custom_watch_s3.cc` 中的全部引脚定义和硬件初始化代码。

| 模块 | 旧引脚 | 新引脚 |
|------|--------|--------|
| QSPI CS/CLK/SIO0/SIO1 | 10/12/11/13 | **3/8/15/16** |
| QSPI TE | 8 | **10** |
| 扬声器 I2S | BCLK=15 WS=16 DOUT=14 | BCLK=**12** WS=**13** DOUT=**11** |
| 麦克风 I2S | BCLK=15 WS=16 DIN=46 | BCLK=**40** WS=**41** DIN=**39** |
| 触摸 RST | 21 | **7** |
| QMI8658 I2C | 独立 SDA=1 SCL=2 | 共用 SDA=**5** SCL=**4** |
| QMI8658 INT1 | 41 | **21** |
| 充电检测 | 无 | CHECK=**1**, STARTUP=**2**, ADC=**14** |
| 音量键 | 无 | VOL+=**47**, VOL-=**48** |

**影响文件**：
- `main/boards/custom-watch-s3/config.h`
- `main/boards/custom-watch-s3/custom_watch_s3.cc`

---

## 问题 2：BMM150 `memset` 下溢导致随机崩溃

**现象**：启用 BMM150 后，系统出现 `StoreProhibited`、`Cache error`、`InstrFetchProhibited` 等各种随机崩溃；禁用 BMM150 则正常。

**根因**：`bmm150.cc:24` 的 `memset` 计算 size 时，`offsetof(bus_) - offsetof(dig_x1_)` 中 `bus_` 在 `dig_x1_` 之前声明，减法结果为负数（下溢为约 4GB），`memset` 写穿整个堆内存。

```cpp
// 修复前（错误）
memset(&dig_x1_, 0, offsetof(Bmm150, bus_) - offsetof(Bmm150, dig_x1_));
// offsetof(bus_)=0, offsetof(dig_x1_)=12 → 0-12 = -12 → 0xFFFFFFF4（~4GB）

// 修复后（正确）
memset(&dig_x1_, 0, offsetof(Bmm150, dig_xy2_) - offsetof(Bmm150, dig_x1_) + sizeof(dig_xy2_));
// 仅清零 dig_x1_ ~ dig_xy2_ 共 12 字节
```

**影响文件**：`main/boards/common/bmm150.cc`

---

## 问题 3：麦克风无声音

**现象**：对话正常建立，但 VAD 检测到完全静音，服务器提示"你睡着啦"后关闭会话。`I2S TX/RX diag` 底层诊断显示 I2S 硬件通路正常。

**根因**：面包板使用 I2S_NUM_0 duplex 模式（TX+RX 共享 BCLK/WS），新 PCB 使用独立 I2S_NUM_0 TX + I2S_NUM_1 RX。`NoAudioCodec::Read` 的 `value >> 12` 提取方式是为 32-bit 单声道 FIFO word 设计的，但 16-bit stereo 模式下每个 32-bit word 包含左右两个声道，位混叠导致提取的音频数据极微弱（仅 ±32，正常应为数千）。

**解决**：自定义 `Read` 方法，直接从 32-bit stereo word 的低 16 位提取左声道数据（MS4030 的 L/R=GND，数据输出在左声道）。

```cpp
int Read(int16_t* dest, int samples) override {
    // ... i2s_channel_read ...
    for (int i = 0; i < n; i++)
        dest[i] = (int16_t)(buf[i] & 0xFFFF);  // 左声道在 bits[15:0]
    return n;
}
```

**影响文件**：`main/boards/custom-watch-s3/custom_watch_s3.cc`

---

## 问题 4：扬声器杂音/无声音

**现象**：换对喇叭插口前完全无声，换对后有声音但都是杂音。

**根因**：

1. **硬件**：PCB 上有两个喇叭插口，插错导致完全不响。
2. **软件**：`NoAudioCodec::Write` 在 16-bit stereo 模式下，32-bit 音频值被 I2S 控制器拆成左右两个 16-bit 声道。基类 Write 只在左声道输出了有效数据，右声道收到随机值，NS4168 同时放大两个声道产生杂音。

**解决**：自定义 `Write` 方法，将相同音频数据填入左右两个声道。

```cpp
int Write(const int16_t* data, int samples) override {
    // 音量计算...
    int16_t audio = (int16_t)(static_cast<int32_t>(temp) >> 16);
    buf[i] = ((int32_t)audio << 16) | ((int32_t)audio & 0xFFFF);  // 左右相同
    // ... i2s_channel_write ...
}
```

**影响文件**：`main/boards/custom-watch-s3/custom_watch_s3.cc`

---

## 问题 5：步数更新导致 task_wdt 超时

**现象**：QMI8658 工作后，步数回调通过 `Application::Schedule` 投递到 main 任务，直接在 main 任务中调用 `lv_label_set_text` 等 LVGL 函数，与 LVGL 渲染任务冲突导致 main 任务卡死，触发 task watchdog。

**解决**：`OnImuStep` 回调只存储步数值，新增 LVGL 定时器每 2 秒从 LVGL 上下文读取并刷新显示。

```cpp
// 回调中只存储
static void OnImuStep(void* user, int steps) {
    self->step_count_ = steps;
}

// LVGL 定时器中刷新
lv_timer_create([](lv_timer_t* t_step) {
    if (b->watch_face_) b->watch_face_->UpdateSteps(b->step_count_);
}, 2000, self);
```

**影响文件**：`main/boards/custom-watch-s3/custom_watch_s3.cc`

---

## 问题 6：计步过于灵敏

**现象**：走几步就显示 20+ 步，静止时偶尔也增加。

**根因**：QMI8658 驱动中步数检测阈值仅 10.8g，微小的手部动作就能触发。

**解决**：逐步调优参数。

| 参数 | 原始值 | 最终值 |
|------|--------|--------|
| 触发阈值 | 10.8g | **13.0g** |
| 释放阈值 | 10.0g | **11.0g** |
| 防抖间隔 | 200ms | **500ms** |

**影响文件**：`main/boards/common/qmi8658.cc`

---

## 问题 7：摇一摇唤醒过于灵敏

**现象**：拿起手表、走路、缓慢摆臂等正常动作都误触发唤醒。

**根因**：经历了三轮算法迭代。

**第一版（方差法）**：10 个样本的 acc_mag 方差 > 30 即触发，单次传感器尖峰（mag=33.97）就触发。且无坏数据过滤。

**第二版（方向变化法）**：连续样本间三轴累积变化 > 4.0g 即计为事件，800ms 内 ≥3 次事件触发。500Hz 采样下相邻样本间变化极小，实际触发靠的是大量随机微小波动。

**第三版（振幅法）**：15 样本窗口（~30ms）内 max-min > 8.0g 触发。拿起/走路也能产生足够振幅。

**最终方案（峰值计数法）**：

```cpp
// 500ms 内 ≥5 次峰值（每次 > 12.0g）+ 5 秒冷却
if (acc_mag > 12.0f && !shake_above) {
    shake_above = true;
    记录峰值时间戳;
    if (500ms内峰值次数 >= 5 && 距上次触发 > 5000ms) {
        触发摇一摇;
    }
}
if (acc_mag < 10.0f) shake_above = false;
```

- 慢速走路/摆臂：500ms 内 1-2 次峰值 → 不触发
- 真正快速摇晃：500ms 内 5+ 次峰值 → 正确触发
- 传感器饱和值（±19.6）：被数据验证过滤

**影响文件**：`main/boards/common/qmi8658.cc`

---

## 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `config.h` | 全部引脚定义重新映射 |
| `custom_watch_s3.cc` | 音频编解码器（Read/Write 重写）、传感器 I2C 合并、电源/按键初始化、步数/摇一摇回调 |
| `bmm150.cc` | 修复 memset 下溢 bug（1 行） |
| `qmi8658.cc` | 计步阈值、摇一摇算法重写、数据验证 |
| `watch_face.cc` | 状态栏背景消除 GRAM 残留、强制刷新 |
