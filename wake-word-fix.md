# 唤醒词"你好小智"失效问题根因分析与修复

## 现象

新板子（ESP32-S3 + NS4168 + MS4030 + CO5300）上，在 idle 状态下说"你好小智"无法唤醒设备。必须按 BOOT 按键手动触发对话。但对话开始后麦克风工作正常（STT 能识别语音内容）。

## 排查过程

| 尝试 | 方法 | 结果 |
|------|------|------|
| 1 | 降低 AFE VAD 模式到 MODE_0 | 无效 |
| 2 | 降低 `vad_min_speech_ms` 从 128ms 到 32ms | 无效 |
| 3 | 关闭 `vad_min_speech_ms` 和 `vad_min_noise_ms`（设为 0） | 无效 |
| 4 | 切换 AFE 模式为 `LOW_COST` | 无效 |
| 5 | **统一输入输出采样率为 16000Hz** | ✅ 唤醒恢复正常 |

## 根因

**I2S 硬件采样率与 AFE 期望采样率不匹配。**

原配置：
```c
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
```

`NoAudioCodecDuplex` 的 I2S 时钟由 `output_sample_rate`（24000Hz）决定。MS4030 麦克风作为 I2S 从机，实际以 24000Hz 输出数据。但 AFE（Audio Front-End）期望 16000Hz 输入。AudioService 做了一次 resample（24000→16000），这个过程破坏了唤醒词所需的音频频谱特征。

```
错误链路：
  MS4030 → I2S(24000Hz) → resample(24000→16000) → AFE VAD → WakeNet ❌ 检测失败

正确链路：
  MS4030 → I2S(16000Hz) → AFE VAD → WakeNet ✅ 检测成功
```

对话模式下麦克风能工作是因为 AfeAudioProcessor 对 resample 误差容忍度更高（只需 VAD 检测人声，不需要精确匹配唤醒词频谱）。

## 修复

```diff
- #define AUDIO_INPUT_SAMPLE_RATE  16000
- #define AUDIO_OUTPUT_SAMPLE_RATE 24000
+ #define AUDIO_INPUT_SAMPLE_RATE  16000
+ #define AUDIO_OUTPUT_SAMPLE_RATE 16000
```

**修改文件**：`main/boards/custom-watch-s3/config.h`

输入输出统一 16000Hz 后：
- I2S BCLK = 16000 × 32 = 512kHz，匹配 MS4030 标称值
- WS 频率 = 16000Hz，AFE 直接处理无需 resample
- 唤醒词秒级响应，两次测试均一次成功

附带 warning：`Server sample rate 24000 does not match device output sample rate 16000`，系统自动适配，不影响功能。
