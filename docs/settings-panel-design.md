# Custom Watch S3 设置面板 设计文档

> 版本: 1.0  
> 日期: 2026-06-03  
> 硬件: ESP32-S3 + CO5300 AMOLED 410×502

---

## 一、概述

在手表现有三页滑动界面的基础上，新增一个**设置面板**。通过状态栏齿轮图标进入，以半透明遮罩+居中卡片形式覆盖显示。设置项均可真实生效，数据持久化到 NVS。

---

## 二、入口设计

### 2.1 齿轮图标

| 属性 | 值 |
|------|-----|
| 位置 | 状态栏右侧（`battery_label_` 右边，约 `x=60, y=3`） |
| 图标 | FontAwesome `"cog"`，20px，白色 |
| 交互 | `LV_EVENT_CLICKED` → `ShowSettings()` |
| 实现 | `lv_label_create(container_)`，同状态栏其他元素 |

### 2.2 面板显示/隐藏

```
┌──────────────────────────────────┐
│  ← 返回              设置       │  ← 标题栏 (20px 灰色)
├──────────────────────────────────┤
│                                  │
│    主题                          │
│    [ 暗色 ]  [ 亮色 ]            │  ← 二选一按钮
│                                  │
│    音量                          │
│    ───●────────────── 85%       │  ← lv_slider 滑动条
│    [ - ]              [ + ]      │  ← 两按钮快速调节
│                                  │
│    唤醒词                        │
│    [ ▼ 你好小智           ]      │  ← lv_dropdown 下拉
│                                  │
│    语音模型                      │
│    [ ▼ 云端默认           ]      │  ← 下拉（信息展示，设备端不切换）
│                                  │
│            [ 保存并重启 ]        │  ← 确认按钮
│                                  │
└──────────────────────────────────┘
   半透明遮罩 410×502 (bg_opa=60%)
   居中卡片 340×360 (圆角 16px)
```

---

## 三、设置项详细设计

### 3.1 主题切换

| 属性 | 值 |
|------|-----|
| 类型 | 两按钮互斥选择 |
| 选项 | `"dark"`（暗色）, `"light"`（亮色） |
| 存储 | `Settings("display").SetString("theme", value)` |
| 生效 | 调用 `display_->SetTheme(LvglThemeManager::GetInstance().GetTheme(value))` |

**实现方式**：
```cpp
// 读取当前主题
Settings settings("display", false);
std::string current = settings.GetString("theme", "dark");

// 创建两个按钮，当前选中高亮
lv_obj_t* btn_dark = lv_btn_create(panel);
lv_obj_t* btn_light = lv_btn_create(panel);
// 选中样式: bg 白色边框, 未选中: 无边框
```

**生效范围**：
- 手表界面（黑色/白色背景）
- 聊天界面（气泡颜色等）
- 立即生效，无需重启

### 3.2 音量调节

| 属性 | 值 |
|------|-----|
| 类型 | `lv_slider` 滑动条 + `-`/`+` 按钮 |
| 范围 | 0 - 100 |
| 步进 | 5（每次加减 5%） |
| 存储 | `Settings("audio").SetInt("output_volume", value)` |
| 生效 | 调用 `board.GetAudioCodec()->SetOutputVolume(value)` |
| PCB 按键 | GPIO 47 (VOL+), GPIO 48 (VOL-) — 长按连续调 |

**实现方式**：
```cpp
// 滑动条
lv_obj_t* slider = lv_slider_create(panel);
lv_slider_set_range(slider, 0, 100);
lv_slider_set_value(slider, current_volume, LV_ANIM_OFF);

// 实时生效
lv_obj_add_event_cb(slider, [](lv_event_t* e) {
    int vol = lv_slider_get_value(lv_event_get_target(e));
    Board::GetInstance().GetAudioCodec()->SetOutputVolume(vol);
}, LV_EVENT_VALUE_CHANGED, nullptr);
```

**PCB 音量按键**（当前禁用，需启用）：
```cpp
// config.h 中已定义:
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_47
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_48

// 回调实现:
volume_up_button_.OnClick([this]() {
    int vol = GetAudioCodec()->output_volume() + 5;
    if (vol > 100) vol = 100;
    GetAudioCodec()->SetOutputVolume(vol);
});
volume_up_button_.OnLongPress([this]() {
    // 连续调大音量
});
```

### 3.3 唤醒词选择

| 属性 | 值 |
|------|-----|
| 类型 | `lv_dropdown` 下拉选择 |
| 选项来源 | 从 `srmodels.bin` 中动态读取 |
| 存储 | `Settings("audio").SetString("wake_word", value)` |
| 生效方式 | 写入 NVS，需重启 |

**当前设备可用唤醒词**（取决于刷入的 `srmodels.bin`）：
- ESP32S3 + AFE 模式下，通过 `esp_srmodel_get_wake_words()` 获取
- 常见选项：`"你好小智"`, `"小智小智"`, `"嘿小智"` 等

**实现方式**：
```cpp
// 从 AFE 获取可用唤醒词列表
std::vector<std::string> wake_words;
// 调用 esp_srmodel_get_wake_words() 或从 index.json 解析

// 填充 dropdown
lv_dropdown_set_options(dropdown, "你好小智\n小智小智\n嘿小智");
// 选中当前唤醒词
lv_dropdown_set_text(dropdown, current_wake_word);
```

### 3.4 语音模型（信息展示）

| 属性 | 值 |
|------|-----|
| 类型 | `lv_dropdown` 下拉（只读展示） |
| 选项 | `"云端默认"` — 模型由服务器端决定 |
| 说明 | 设备端不支持切换语音模型，此栏仅展示信息 |

**注意**：语音模型（LLM/TTS/ASR）的选择完全由云端服务器在连接激活时协商决定，设备端只负责音频采集和播放。此设置项为只读展示，提示用户模型可从小智 App/Web 后台配置。

---

## 四、数据结构

### 4.1 NVS 存储

| Namespace | Key | 类型 | 默认值 | 用途 |
|-----------|-----|------|--------|------|
| `"display"` | `"theme"` | String | `"dark"` | 主题 |
| `"audio"` | `"output_volume"` | Int | `70` | 音量 |
| `"audio"` | `"wake_word"` | String | `"你好小智"` | 唤醒词 |

### 4.2 新增 WatchFace 成员

```cpp
// 设置面板
lv_obj_t* settings_overlay_;      // 半透明遮罩
lv_obj_t* settings_panel_;        // 居中卡片
lv_obj_t* settings_slider_;       // 音量滑动条
lv_obj_t* settings_vol_label_;    // 音量数值标签
lv_obj_t* settings_theme_dark_;   // 暗色按钮
lv_obj_t* settings_theme_light_;  // 亮色按钮
lv_obj_t* settings_wake_dd_;      // 唤醒词下拉
lv_obj_t* settings_cog_;          // 状态栏齿轮入口
```

---

## 五、文件修改清单

| 文件 | 修改内容 | 工作量 |
|------|----------|--------|
| `watch_face.h` | 新增设置面板成员变量和方法声明 | ~20行 |
| `watch_face.cc` | 实现 `CreateSettingsPanel()`, `ShowSettings()`, `HideSettings()`, 齿轮图标创建和事件 | ~150行 |
| `custom_watch_s3.cc` | 启用音量按键 GPIO，绑定回调；主题切换实际生效 | ~30行 |

---

## 六、交互流程

```
用户点击齿轮图标
  → ShowSettings()
    → 创建遮罩 + 面板（如果未创建）
    → 面板从底部滑入动画（可选）
    → 读取当前设置值填充控件

用户调节音量滑块
  → LV_EVENT_VALUE_CHANGED
    → AudioCodec::SetOutputVolume(val)
    → 实时生效，存入 NVS

用户切换主题按钮
  → LV_EVENT_CLICKED
    → 更新按钮高亮状态
    → display_->SetTheme(theme)
    → 立即生效

用户选择唤醒词
  → LV_EVENT_VALUE_CHANGED (dropdown)
    → 标记为待保存

用户点击「保存并重启」
  → 写入所有设置到 NVS
  → Application::Reboot()
```

---

## 七、注意事项

1. **唤醒词切换需重启**：AFE 唤醒词在音频服务启动时加载，运行时切换需要重启才生效
2. **音量 PCB 按键**：当前 `volume_up_button_`/`volume_down_button_` 的 GPIO 设为 `GPIO_NUM_NC`，需改为实际 GPIO 47/48 并实现回调
3. **设置面板层级**：面板需在 `container_` 的最上层（最后创建），z-order 高于三页滚动容器
4. **滑动冲突**：面板显示时需禁用底部页面滑动，隐藏后恢复
