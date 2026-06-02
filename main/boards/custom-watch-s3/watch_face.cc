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

static const lv_color_t C_BG       = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t C_WHITE    = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t C_GRAY     = LV_COLOR_MAKE(0x99, 0x99, 0x99);
static const lv_color_t C_ORANGE   = LV_COLOR_MAKE(0xFF, 0x95, 0x00);
static const lv_color_t C_DARK     = LV_COLOR_MAKE(0x33, 0x33, 0x33);
static const lv_color_t C_DIMMED   = LV_COLOR_MAKE(0x55, 0x55, 0x55);
static const lv_color_t C_DARKER   = LV_COLOR_MAKE(0x44, 0x44, 0x44);

// ==================== 构造函数 ====================

WatchFace::WatchFace(lv_obj_t* parent)
    : current_page_(0), compass_heading_(0)
{
    container_ = lv_obj_create(parent);
    lv_obj_remove_style_all(container_);
    lv_obj_set_size(container_, 410, 502);
    lv_obj_set_style_bg_color(container_, C_BG, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_center(container_);

    CreateScrollLayout();
    CreateHomePage();
    CreateStepsPage();
    CreateCompassPage();
    CreatePageIndicator();
    CreateStatusBar();

    clock_timer_  = lv_timer_create(ClockTimerCB, 1000, this);
    status_timer_ = lv_timer_create(StatusTimerCB, 5000, this);
    UpdateClock();
    UpdateStatusBar();

    ESP_LOGI(TAG, "WatchFace created (3 pages)");
}

WatchFace::~WatchFace() {
    if (clock_timer_)  lv_timer_delete(clock_timer_);
    if (status_timer_) lv_timer_delete(status_timer_);
    if (container_)    lv_obj_delete(container_);
}

// ==================== Scroll Layout ====================

void WatchFace::CreateScrollLayout() {
    scroll_container_ = lv_obj_create(container_);
    lv_obj_remove_style_all(scroll_container_);
    lv_obj_set_size(scroll_container_, 410, 502);
    lv_obj_set_scroll_dir(scroll_container_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(scroll_container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(scroll_container_, LV_SCROLL_SNAP_START);
    lv_obj_add_flag(scroll_container_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_bg_opa(scroll_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_container_, 0, 0);
    lv_obj_set_style_pad_all(scroll_container_, 0, 0);
    lv_obj_set_content_width(scroll_container_, 1230);

    for (int i = 0; i < 3; i++) {
        page_[i] = lv_obj_create(scroll_container_);
        lv_obj_remove_style_all(page_[i]);
        lv_obj_set_size(page_[i], 410, 502);
        lv_obj_set_pos(page_[i], i * 410, 0);
        lv_obj_set_style_bg_opa(page_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page_[i], 0, 0);
        lv_obj_set_style_pad_all(page_[i], 0, 0);
    }

    lv_obj_add_event_cb(scroll_container_, ScrollEventCB, LV_EVENT_SCROLL_END, this);
}

// ==================== 页面指示器 ====================

void WatchFace::CreatePageIndicator() {
    for (int i = 0; i < 3; i++) {
        page_dot_[i] = lv_obj_create(container_);
        lv_obj_remove_style_all(page_dot_[i]);
        lv_obj_set_size(page_dot_[i], 8, 8);
        lv_obj_set_pos(page_dot_[i], 185 + i * 20, 445);
        lv_obj_set_style_radius(page_dot_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(page_dot_[i],
            i == 0 ? C_WHITE : C_DARKER, 0);
        lv_obj_set_style_bg_opa(page_dot_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(page_dot_[i], 0, 0);
    }
}

void WatchFace::UpdatePageIndicator() {
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(page_dot_[i],
            i == current_page_ ? C_WHITE : C_DARKER, 0);
    }
}

void WatchFace::ScrollEventCB(lv_event_t* e) {
    auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
    lv_obj_t* scroll = lv_event_get_target_obj(e);
    int scroll_x = lv_obj_get_scroll_x(scroll);
    int page = (scroll_x + 205) / 410;
    if (page < 0) page = 0;
    if (page > 2) page = 2;
    if (page != self->current_page_) {
        self->current_page_ = page;
        self->UpdatePageIndicator();
    }
}

// ==================== Page 0: 主页 ====================

void WatchFace::CreateHomePage() {
    auto* p = page_[0];

    // 唤醒按钮 (底部)
    tap_area_[0] = lv_obj_create(p);
    lv_obj_remove_style_all(tap_area_[0]);
    lv_obj_set_size(tap_area_[0], 410, 50);
    lv_obj_align(tap_area_[0], LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tap_area_[0], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap_area_[0], 0, 0);
    lv_obj_add_flag(tap_area_[0], LV_OBJ_FLAG_CLICKABLE);

    tap_hint_[0] = lv_label_create(p);
    lv_obj_set_style_text_font(tap_hint_[0], &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(tap_hint_[0], C_GRAY, 0);
    lv_label_set_text(tap_hint_[0], "唤醒语音聊天");
    lv_obj_align(tap_hint_[0], LV_ALIGN_BOTTOM_MID, 0, -8);

    // 时钟 (30px 放大1.6x，pivot居中)
    clock_label_ = lv_label_create(p);
    lv_obj_set_style_text_font(clock_label_, &font_puhui_30_4, 0);
    lv_obj_set_style_text_color(clock_label_, C_WHITE, 0);
    lv_obj_set_style_transform_pivot_x(clock_label_, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(clock_label_, lv_pct(50), 0);
    lv_obj_set_style_transform_scale_x(clock_label_, 410, 0);
    lv_obj_set_style_transform_scale_y(clock_label_, 410, 0);
    lv_label_set_text(clock_label_, "--:--");
    lv_obj_align(clock_label_, LV_ALIGN_CENTER, 0, -85);

    // 日期 (30px 橙色)
    date_label_ = lv_label_create(p);
    lv_obj_set_style_text_font(date_label_, &font_puhui_30_4, 0);
    lv_obj_set_style_text_color(date_label_, C_ORANGE, 0);
    lv_label_set_text(date_label_, "--");
    lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, -35);

    // 天气图标 (30px FA，左移)
    weather_icon_ = lv_label_create(p);
    lv_obj_set_style_text_font(weather_icon_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(weather_icon_, C_WHITE, 0);
    lv_label_set_text(weather_icon_, "");
    lv_obj_align(weather_icon_, LV_ALIGN_CENTER, -65, 35);

    // 天气文字 (30px)
    weather_label_ = lv_label_create(p);
    lv_obj_set_style_text_font(weather_label_, &font_puhui_30_4, 0);
    lv_obj_set_style_text_color(weather_label_, C_GRAY, 0);
    lv_label_set_text(weather_label_, "");
    lv_obj_align(weather_label_, LV_ALIGN_CENTER, 35, 35);
}

// ==================== Page 1: 步数页 ====================

void WatchFace::CreateStepsPage() {
    auto* p = page_[1];

    // 标题
    lv_obj_t* title = lv_label_create(p);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(title, C_GRAY, 0);
    lv_label_set_text(title, "今日步数");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    // 弧形进度条
    steps_arc_ = lv_arc_create(p);
    lv_obj_set_size(steps_arc_, 210, 210);
    lv_obj_align(steps_arc_, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_range(steps_arc_, 0, 10000);
    lv_arc_set_bg_start_angle(steps_arc_, 135);
    lv_arc_set_bg_end_angle(steps_arc_, 405);
    lv_arc_set_start_angle(steps_arc_, 135);
    lv_obj_set_style_arc_color(steps_arc_, C_ORANGE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(steps_arc_, C_DARK,  LV_PART_MAIN);
    lv_obj_set_style_arc_width(steps_arc_, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(steps_arc_, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(steps_arc_, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(steps_arc_, true, LV_PART_MAIN);
    lv_obj_remove_style(steps_arc_, NULL, LV_PART_KNOB);

    // 步数大数字
    steps_count_label_ = lv_label_create(p);
    lv_obj_set_style_text_font(steps_count_label_, &font_puhui_30_4, 0);
    lv_obj_set_style_text_color(steps_count_label_, C_WHITE, 0);
    lv_label_set_text(steps_count_label_, "0");
    lv_obj_align(steps_count_label_, LV_ALIGN_CENTER, 0, -25);

    // "步" 标签
    lv_obj_t* unit = lv_label_create(p);
    lv_obj_set_style_text_font(unit, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(unit, C_GRAY, 0);
    lv_label_set_text(unit, "步");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 10);

    // 目标
    steps_target_label_ = lv_label_create(p);
    lv_obj_set_style_text_font(steps_target_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(steps_target_label_, C_GRAY, 0);
    lv_label_set_text(steps_target_label_, "目标 10000 步");
    lv_obj_align(steps_target_label_, LV_ALIGN_CENTER, 0, 80);

    // 唤醒按钮
    tap_area_[1] = lv_obj_create(p);
    lv_obj_remove_style_all(tap_area_[1]);
    lv_obj_set_size(tap_area_[1], 410, 56);
    lv_obj_align(tap_area_[1], LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tap_area_[1], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap_area_[1], 0, 0);
    lv_obj_add_flag(tap_area_[1], LV_OBJ_FLAG_CLICKABLE);

    tap_hint_[1] = lv_label_create(p);
    lv_obj_set_style_text_font(tap_hint_[1], &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(tap_hint_[1], C_GRAY, 0);
    lv_label_set_text(tap_hint_[1], "唤醒语音聊天");
    lv_obj_align(tap_hint_[1], LV_ALIGN_BOTTOM_MID, 0, -12);
}

// ==================== Page 2: 指南针 ====================

void WatchFace::CreateCompassPage() {
    auto* p = page_[2];
    int cx = 205, cy = 210;

    // 标题
    lv_obj_t* title = lv_label_create(p);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(title, C_GRAY, 0);
    lv_label_set_text(title, "指南针");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    // 外圈
    compass_ring_outer_ = lv_obj_create(p);
    lv_obj_remove_style_all(compass_ring_outer_);
    lv_obj_set_size(compass_ring_outer_, 190, 190);
    lv_obj_set_pos(compass_ring_outer_, cx - 95, cy - 95);
    lv_obj_set_style_bg_opa(compass_ring_outer_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(compass_ring_outer_, 3, 0);
    lv_obj_set_style_border_color(compass_ring_outer_, C_DIMMED, 0);
    lv_obj_set_style_radius(compass_ring_outer_, LV_RADIUS_CIRCLE, 0);

    // 内圈
    compass_ring_inner_ = lv_obj_create(p);
    lv_obj_remove_style_all(compass_ring_inner_);
    lv_obj_set_size(compass_ring_inner_, 172, 172);
    lv_obj_set_pos(compass_ring_inner_, cx - 86, cy - 86);
    lv_obj_set_style_bg_opa(compass_ring_inner_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(compass_ring_inner_, 1, 0);
    lv_obj_set_style_border_color(compass_ring_inner_, C_DARKER, 0);
    lv_obj_set_style_radius(compass_ring_inner_, LV_RADIUS_CIRCLE, 0);

    // 16条刻度线
    for (int i = 0; i < 16; i++) {
        float angle_deg = i * 22.5f;
        float rad = (angle_deg - 90.0f) * M_PI / 180.0f;
        float cos_a = cosf(rad), sin_a = sinf(rad);

        int r_inner, r_outer, width;
        lv_color_t color;
        if (i % 4 == 0) {
            r_inner = 58; r_outer = 90; width = 3; color = C_WHITE;
        } else if (i % 2 == 0) {
            r_inner = 64; r_outer = 90; width = 2; color = C_DIMMED;
        } else {
            r_inner = 74; r_outer = 90; width = 1; color = C_DARKER;
        }

        compass_tick_pts_[i][0].x = cx + (int)(r_inner * cos_a);
        compass_tick_pts_[i][0].y = cy + (int)(r_inner * sin_a);
        compass_tick_pts_[i][1].x = cx + (int)(r_outer * cos_a);
        compass_tick_pts_[i][1].y = cy + (int)(r_outer * sin_a);

        compass_tick_[i] = lv_line_create(p);
        lv_line_set_points(compass_tick_[i], compass_tick_pts_[i], 2);
        lv_obj_set_style_line_color(compass_tick_[i], color, 0);
        lv_obj_set_style_line_width(compass_tick_[i], width, 0);
        lv_obj_set_style_line_rounded(compass_tick_[i], true, 0);
    }

    // 方向标签 (cx=205 cy=210 radius=95, 全部20px)
    compass_lbl_n_ = lv_label_create(p);
    lv_obj_set_style_text_font(compass_lbl_n_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(compass_lbl_n_, C_ORANGE, 0);
    lv_label_set_text(compass_lbl_n_, "N");
    lv_obj_align(compass_lbl_n_, LV_ALIGN_CENTER, 0, -160);

    compass_lbl_s_ = lv_label_create(p);
    lv_obj_set_style_text_font(compass_lbl_s_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(compass_lbl_s_, C_GRAY, 0);
    lv_label_set_text(compass_lbl_s_, "S");
    lv_obj_align(compass_lbl_s_, LV_ALIGN_CENTER, 0, 75);

    compass_lbl_e_ = lv_label_create(p);
    lv_obj_set_style_text_font(compass_lbl_e_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(compass_lbl_e_, C_GRAY, 0);
    lv_label_set_text(compass_lbl_e_, "E");
    lv_obj_align(compass_lbl_e_, LV_ALIGN_CENTER, 108, -40);

    compass_lbl_w_ = lv_label_create(p);
    lv_obj_set_style_text_font(compass_lbl_w_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(compass_lbl_w_, C_GRAY, 0);
    lv_label_set_text(compass_lbl_w_, "W");
    lv_obj_align(compass_lbl_w_, LV_ALIGN_CENTER, -108, -40);

    // === 北向杆 (白色, 8×75, pivot在底端圆心) ===
    {
        compass_pointer_ = lv_obj_create(p);
        lv_obj_remove_style_all(compass_pointer_);
        lv_obj_set_size(compass_pointer_, 8, 75);
        lv_obj_set_pos(compass_pointer_, cx - 4, cy - 75);
        lv_obj_set_style_bg_color(compass_pointer_, C_WHITE, 0);
        lv_obj_set_style_bg_opa(compass_pointer_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(compass_pointer_, 4, 0);
        lv_obj_set_style_border_width(compass_pointer_, 0, 0);
        lv_obj_set_style_transform_pivot_x(compass_pointer_, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(compass_pointer_, lv_pct(100), 0);
    }

    // === 南向杆 (橙色, 8×75, pivot在底端圆心) ===
    {
        compass_pointer_south_ = lv_obj_create(p);
        lv_obj_remove_style_all(compass_pointer_south_);
        lv_obj_set_size(compass_pointer_south_, 8, 75);
        lv_obj_set_pos(compass_pointer_south_, cx - 4, cy - 75);
        lv_obj_set_style_bg_color(compass_pointer_south_, C_ORANGE, 0);
        lv_obj_set_style_bg_opa(compass_pointer_south_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(compass_pointer_south_, 4, 0);
        lv_obj_set_style_border_width(compass_pointer_south_, 0, 0);
        lv_obj_set_style_transform_pivot_x(compass_pointer_south_, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(compass_pointer_south_, lv_pct(100), 0);
        lv_obj_set_style_transform_rotation(compass_pointer_south_, 1800, 0);
    }

    // 中心点
    compass_dot_ = lv_obj_create(p);
    lv_obj_remove_style_all(compass_dot_);
    lv_obj_set_size(compass_dot_, 6, 6);
    lv_obj_set_pos(compass_dot_, cx - 3, cy - 3);
    lv_obj_set_style_bg_color(compass_dot_, C_ORANGE, 0);
    lv_obj_set_style_bg_opa(compass_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(compass_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(compass_dot_, 0, 0);

    // 方位角文字
    compass_heading_label_ = lv_label_create(p);
    lv_obj_set_style_text_font(compass_heading_label_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(compass_heading_label_, C_WHITE, 0);
    lv_label_set_text(compass_heading_label_, "--");
    lv_obj_align(compass_heading_label_, LV_ALIGN_CENTER, 0, 135);

    // 唤醒按钮
    tap_area_[2] = lv_obj_create(p);
    lv_obj_remove_style_all(tap_area_[2]);
    lv_obj_set_size(tap_area_[2], 410, 56);
    lv_obj_align(tap_area_[2], LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tap_area_[2], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap_area_[2], 0, 0);
    lv_obj_add_flag(tap_area_[2], LV_OBJ_FLAG_CLICKABLE);

    tap_hint_[2] = lv_label_create(p);
    lv_obj_set_style_text_font(tap_hint_[2], &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(tap_hint_[2], C_GRAY, 0);
    lv_label_set_text(tap_hint_[2], "唤醒语音聊天");
    lv_obj_align(tap_hint_[2], LV_ALIGN_BOTTOM_MID, 0, -12);

    // 默认指向北
    UpdateCompassPointer();
}

// ==================== 状态栏 ====================

void WatchFace::CreateStatusBar() {
    status_bar_bg_ = lv_obj_create(container_);
    lv_obj_remove_style_all(status_bar_bg_);
    lv_obj_set_size(status_bar_bg_, 410, 26);
    lv_obj_set_pos(status_bar_bg_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_bg_, C_BG, 0);
    lv_obj_set_style_bg_opa(status_bar_bg_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_bar_bg_, 0, 0);
    lv_obj_set_style_radius(status_bar_bg_, 0, 0);

    wifi_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(wifi_label_, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(wifi_label_, C_WHITE, 0);
    lv_label_set_text(wifi_label_, font_awesome_get_utf8("wifi_slash"));
    lv_obj_align(wifi_label_, LV_ALIGN_TOP_MID, -40, 3);

    battery_icon_ = lv_label_create(container_);
    lv_obj_set_style_text_font(battery_icon_, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(battery_icon_, C_WHITE, 0);
    lv_label_set_text(battery_icon_, font_awesome_get_utf8("battery_full"));
    lv_obj_align(battery_icon_, LV_ALIGN_TOP_MID, 0, 3);

    battery_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(battery_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(battery_label_, C_WHITE, 0);
    lv_label_set_text(battery_label_, "--%");
    lv_obj_align(battery_label_, LV_ALIGN_TOP_MID, 40, 3);
}

void WatchFace::UpdateStatusBar() {
    bool wifi_ok = false;
    auto* app = &Application::GetInstance();
    if (app) {
        auto state = app->GetDeviceState();
        wifi_ok = (state == kDeviceStateIdle || state == kDeviceStateListening ||
                   state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
    }
    lv_label_set_text(wifi_label_, font_awesome_get_utf8(wifi_ok ? "wifi" : "wifi_slash"));
    lv_obj_set_style_text_color(wifi_label_, wifi_ok ? C_WHITE : lv_color_hex(0x666666), 0);

    lv_label_set_text(battery_icon_, font_awesome_get_utf8("battery_full"));
    lv_label_set_text(battery_label_, "100%");
}

void WatchFace::StatusTimerCB(lv_timer_t* timer) {
    auto* self = static_cast<WatchFace*>(lv_timer_get_user_data(timer));
    self->UpdateStatusBar();
}

// ==================== 显隐 ====================

void WatchFace::Show() {
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
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
    if (steps > 10000) steps = 10000;
    lv_arc_set_value(steps_arc_, steps);

    char buf[32];
    if (steps < 10000) {
        snprintf(buf, sizeof(buf), "%d", steps);
    } else {
        snprintf(buf, sizeof(buf), "10000+");
    }
    lv_label_set_text(steps_count_label_, buf);
}

// ==================== 指南针 ====================

void WatchFace::UpdateCompass(float heading_deg) {
    compass_heading_ = heading_deg;
    UpdateCompassPointer();
}

void WatchFace::UpdateCompassPointer() {
    int32_t h = (int32_t)(compass_heading_ * 10);
    lv_obj_set_style_transform_rotation(compass_pointer_, h, 0);
    lv_obj_set_style_transform_rotation(compass_pointer_south_, h + 1800, 0);

    const char* dirs[] = {"北","东北","东","东南","南","西南","西","西北"};
    int idx = ((int)(compass_heading_ + 22.5f) / 45) % 8;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s  %d°", dirs[idx], (int)compass_heading_);
    lv_label_set_text(compass_heading_label_, buf);
}
