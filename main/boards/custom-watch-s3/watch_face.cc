#include "watch_face.h"
#include "application.h"
#include "device_state.h"

#include <font_awesome.h>
#include <esp_log.h>
#include <cstdio>
#include <cmath>
#include <ctime>

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

#define TAG "WatchFace"

static const lv_color_t C_BG     = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t C_WHITE  = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t C_GRAY   = LV_COLOR_MAKE(0x99, 0x99, 0x99);
static const lv_color_t C_ORANGE = LV_COLOR_MAKE(0xFF, 0x95, 0x00);

// ==================== 构造函数 ====================

WatchFace::WatchFace(lv_obj_t* parent)
    : compass_heading_(0)
{
    // 全屏容器
    container_ = lv_obj_create(parent);
    lv_obj_remove_style_all(container_);
    lv_obj_set_size(container_, 410, 502);
    lv_obj_set_style_bg_color(container_, C_BG, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_center(container_);
    lv_obj_move_foreground(container_);

    // ===== 状态栏：WiFi + 电池 =====
    CreateStatusBar();

    // ===== 时钟 30px 白色 =====
    clock_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(clock_label_, &font_puhui_30_4, 0);
    lv_obj_set_style_text_color(clock_label_, C_WHITE, 0);
    lv_label_set_text(clock_label_, "--:--");
    lv_obj_align(clock_label_, LV_ALIGN_TOP_MID, 0, 55);

    // ===== 日期 20px 橙色，中文 =====
    date_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(date_label_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(date_label_, C_ORANGE, 0);
    lv_label_set_text(date_label_, "--");
    lv_obj_align(date_label_, LV_ALIGN_TOP_MID, 0, 115);

    // ===== 天气图标 30px FontAwesome =====
    weather_icon_ = lv_label_create(container_);
    lv_obj_set_style_text_font(weather_icon_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(weather_icon_, C_WHITE, 0);
    lv_label_set_text(weather_icon_, "");
    lv_obj_align(weather_icon_, LV_ALIGN_TOP_MID, -60, 155);

    // ===== 天气文字 20px 灰色 =====
    weather_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(weather_label_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(weather_label_, C_GRAY, 0);
    lv_label_set_text(weather_label_, "");
    lv_obj_align(weather_label_, LV_ALIGN_TOP_MID, 30, 155);

    // ===== 指南针 160x160 圆盘 =====
    CreateCompass(205, 300, 70);

    // ===== 步数 20px 灰色 =====
    steps_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(steps_label_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(steps_label_, C_GRAY, 0);
    lv_label_set_text(steps_label_, "0 步");
    lv_obj_align(steps_label_, LV_ALIGN_BOTTOM_MID, 0, -60);

    // ===== 点击区域 + 提示 =====
    tap_area_ = lv_obj_create(container_);
    lv_obj_set_size(tap_area_, 410, 50);
    lv_obj_align(tap_area_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tap_area_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap_area_, 0, 0);

    tap_hint_ = lv_label_create(container_);
    lv_obj_set_style_text_font(tap_hint_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(tap_hint_, C_GRAY, 0);
    lv_label_set_text(tap_hint_, "唤醒语音聊天");
    lv_obj_align(tap_hint_, LV_ALIGN_BOTTOM_MID, 0, -10);

    // 1秒刷新时钟
    clock_timer_ = lv_timer_create(ClockTimerCB, 1000, this);
    UpdateClock();

    // 5秒刷新状态栏
    status_timer_ = lv_timer_create(StatusTimerCB, 5000, this);
    UpdateStatusBar();

    ESP_LOGI(TAG, "WatchFace created");
}

// ==================== 指南针创建 ====================

void WatchFace::CreateCompass(int cx, int cy, int radius) {
    // 父容器
    compass_cont_ = lv_obj_create(container_);
    lv_obj_remove_style_all(compass_cont_);
    lv_obj_set_size(compass_cont_, radius * 2 + 20, radius * 2 + 20);
    lv_obj_set_style_bg_opa(compass_cont_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(compass_cont_, 0, 0);
    lv_obj_set_style_pad_all(compass_cont_, 0, 0);
    lv_obj_set_pos(compass_cont_, cx - radius - 10, cy - radius - 10);

    int mid = radius + 10;  // 容器中心

    // 圆环
    compass_ring_ = lv_obj_create(compass_cont_);
    lv_obj_remove_style_all(compass_ring_);
    lv_obj_set_size(compass_ring_, radius * 2, radius * 2);
    lv_obj_set_style_bg_opa(compass_ring_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(compass_ring_, 2, 0);
    lv_obj_set_style_border_color(compass_ring_, lv_color_hex(0x666666), 0);
    lv_obj_set_style_radius(compass_ring_, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(compass_ring_);

    // 方向标签
    compass_lbl_n_ = lv_label_create(compass_cont_);
    lv_obj_set_style_text_font(compass_lbl_n_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(compass_lbl_n_, C_ORANGE, 0);
    lv_label_set_text(compass_lbl_n_, "N");
    lv_obj_align(compass_lbl_n_, LV_ALIGN_TOP_MID, 0, -2);

    compass_lbl_s_ = lv_label_create(compass_cont_);
    lv_obj_set_style_text_font(compass_lbl_s_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(compass_lbl_s_, C_GRAY, 0);
    lv_label_set_text(compass_lbl_s_, "S");
    lv_obj_align(compass_lbl_s_, LV_ALIGN_BOTTOM_MID, 0, 2);

    compass_lbl_e_ = lv_label_create(compass_cont_);
    lv_obj_set_style_text_font(compass_lbl_e_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(compass_lbl_e_, C_GRAY, 0);
    lv_label_set_text(compass_lbl_e_, "E");
    lv_obj_align(compass_lbl_e_, LV_ALIGN_RIGHT_MID, -2, 0);

    compass_lbl_w_ = lv_label_create(compass_cont_);
    lv_obj_set_style_text_font(compass_lbl_w_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(compass_lbl_w_, C_GRAY, 0);
    lv_label_set_text(compass_lbl_w_, "W");
    lv_obj_align(compass_lbl_w_, LV_ALIGN_LEFT_MID, 2, 0);

    // 圆心点
    compass_dot_ = lv_obj_create(compass_cont_);
    lv_obj_remove_style_all(compass_dot_);
    lv_obj_set_size(compass_dot_, 8, 8);
    lv_obj_set_style_bg_color(compass_dot_, C_ORANGE, 0);
    lv_obj_set_style_bg_opa(compass_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(compass_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(compass_dot_, 0, 0);
    lv_obj_center(compass_dot_);

    // 指针线
    compass_pointer_ = lv_line_create(compass_cont_);
    lv_obj_set_style_line_color(compass_pointer_, C_ORANGE, 0);
    lv_obj_set_style_line_width(compass_pointer_, 3, 0);
    lv_obj_set_style_line_rounded(compass_pointer_, true, 0);
    compass_points_[0] = {mid, mid};  // 中心
    compass_points_[1] = {mid, mid - radius + 6};  // 初始指北
    lv_line_set_points(compass_pointer_, compass_points_, 2);
}

void WatchFace::UpdateCompassPointer() {
    float rad = (compass_heading_ - 90) * M_PI / 180.0f;
    int mid = 80;  // (160+20)/2 = 90... wait, let me recalculate
    // Actually the container is (radius*2+20) = 160, center = 90
    mid = 90;
    int len = 60;
    compass_points_[0] = {mid, mid};
    compass_points_[1] = {mid + (int)(len * cosf(rad)), mid + (int)(len * sinf(rad))};
    lv_line_set_points(compass_pointer_, compass_points_, 2);
}

// ==================== 状态栏 ====================

void WatchFace::CreateStatusBar() {
    // 状态栏纯黑背景条（宽度覆盖全屏，确保消除GRAM白条残留）
    lv_obj_t* status_bg = lv_obj_create(container_);
    lv_obj_remove_style_all(status_bg);
    lv_obj_set_size(status_bg, 410, 24);
    lv_obj_set_pos(status_bg, 0, 0);
    lv_obj_set_style_bg_color(status_bg, C_BG, 0);
    lv_obj_set_style_bg_opa(status_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_bg, 0, 0);
    lv_obj_set_style_radius(status_bg, 0, 0);

    // WiFi 图标 — 右侧，20px，往中间靠
    wifi_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(wifi_label_, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(wifi_label_, C_WHITE, 0);
    lv_label_set_text(wifi_label_, font_awesome_get_utf8("wifi_slash"));
    lv_obj_align(wifi_label_, LV_ALIGN_TOP_RIGHT, -110, 2);

    // 电池图标
    battery_icon_ = lv_label_create(container_);
    lv_obj_set_style_text_font(battery_icon_, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(battery_icon_, C_WHITE, 0);
    lv_label_set_text(battery_icon_, font_awesome_get_utf8("battery_full"));
    lv_obj_align(battery_icon_, LV_ALIGN_TOP_RIGHT, -70, 2);

    // 电池百分比
    battery_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(battery_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(battery_label_, C_WHITE, 0);
    lv_label_set_text(battery_label_, "--%");
    lv_obj_align(battery_label_, LV_ALIGN_TOP_RIGHT, -35, 2);
}

void WatchFace::UpdateStatusBar() {
    // WiFi 状态
    bool wifi_ok = false;
    auto* app = &Application::GetInstance();
    if (app) {
        auto state = app->GetDeviceState();
        wifi_ok = (state == kDeviceStateIdle || state == kDeviceStateListening ||
                   state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
    }
    lv_label_set_text(wifi_label_, font_awesome_get_utf8(wifi_ok ? "wifi" : "wifi_slash"));
    lv_obj_set_style_text_color(wifi_label_, wifi_ok ? C_WHITE : lv_color_hex(0x666666), 0);

    // 电池（占位：无电池监控硬件，固定显示 100%）
    lv_label_set_text(battery_icon_, font_awesome_get_utf8("battery_full"));
    lv_label_set_text(battery_label_, "100%");
}

void WatchFace::StatusTimerCB(lv_timer_t* timer) {
    auto* self = static_cast<WatchFace*>(lv_timer_get_user_data(timer));
    self->UpdateStatusBar();
}

// ==================== 析构 ====================

WatchFace::~WatchFace() {
    if (clock_timer_) lv_timer_delete(clock_timer_);
    if (status_timer_) lv_timer_delete(status_timer_);
    if (container_) lv_obj_delete(container_);
}

// ==================== 显隐 ====================

void WatchFace::Show() {
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    // 强制全屏刷新：PARTIAL模式下GRAM可能残留聊天UI数据
    lv_obj_invalidate(container_);
}

void WatchFace::Hide() {
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
}

// ==================== 时钟 ====================

void WatchFace::ClockTimerCB(lv_timer_t* timer) {
    auto* self = static_cast<WatchFace*>(lv_timer_get_user_data(timer));
    self->UpdateClock();
}

void WatchFace::UpdateClock() {
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    if (tm->tm_year + 1900 < 2025) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    lv_label_set_text(clock_label_, buf);

    const char* wdays[] = {"周日","周一","周二","周三","周四","周五","周六"};
    snprintf(buf, sizeof(buf), "%d月%d日 %s", tm->tm_mon + 1, tm->tm_mday, wdays[tm->tm_wday]);
    lv_label_set_text(date_label_, buf);
}

// ==================== 天气 ====================

const char* WatchFace::WeatherIcon(const char* desc) {
    if (!desc) return "sun";
    if (strstr(desc, "Thunder")||strstr(desc,"雷")) return "cloud_bolt";
    if (strstr(desc, "Rain")||strstr(desc,"雨")||strstr(desc,"Shower")||strstr(desc,"Drizzle")) return "cloud_rain";
    if (strstr(desc, "Snow")||strstr(desc,"雪")) return "snowflake";
    if (strstr(desc, "Mist")||strstr(desc,"Fog")||strstr(desc,"雾")||strstr(desc,"Haze")) return "smog";
    if (strstr(desc, "Overcast")) return "clouds";
    if (strstr(desc, "Cloud")||strstr(desc,"云")||strstr(desc,"Partly")) return "cloud_sun";
    if (strstr(desc, "Sun")||strstr(desc,"晴")||strstr(desc,"Clear")) return "sun";
    return "sun";
}

void WatchFace::UpdateWeather(const char* desc, int temp_c) {
    const char* fa_icon = WeatherIcon(desc);
    const char* fa_utf8 = font_awesome_get_utf8(fa_icon);
    if (fa_utf8 && fa_utf8[0]) {
        lv_label_set_text(weather_icon_, fa_utf8);
    }

    // 英文描述翻译成中文
    const char* cn_desc = desc ? desc : "";
    if (strstr(desc, "Sunny")||strstr(desc,"Clear")) cn_desc = "晴";
    else if (strstr(desc, "Partly")||strstr(desc,"Cloud")) cn_desc = "多云";
    else if (strstr(desc, "Overcast")) cn_desc = "阴";
    else if (strstr(desc, "Rain")||strstr(desc,"Drizzle")||strstr(desc,"Shower")) cn_desc = "雨";
    else if (strstr(desc, "Thunder")) cn_desc = "雷雨";
    else if (strstr(desc, "Snow")) cn_desc = "雪";
    else if (strstr(desc, "Mist")||strstr(desc,"Fog")||strstr(desc,"Haze")) cn_desc = "雾";

    char buf[64];
    snprintf(buf, sizeof(buf), "%s  %d°C", cn_desc, temp_c);
    lv_label_set_text(weather_label_, buf);
}

// ==================== 步数 ====================

void WatchFace::UpdateSteps(int steps) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d 步", steps >= 0 ? steps : 0);
    lv_label_set_text(steps_label_, buf);
}

// ==================== 指南针 ====================

void WatchFace::UpdateCompass(float heading_deg) {
    compass_heading_ = heading_deg;
    UpdateCompassPointer();
}
