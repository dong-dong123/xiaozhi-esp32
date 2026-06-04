#include "watch_face.h"
#include "application.h"
#include "board.h"
#include "device_state.h"
#include "settings.h"
#include "lvgl_theme.h"

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
    : current_page_(0), compass_heading_(0), settings_volume_(70)
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
    CreateSettingsPanel();

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
    // 禁止滚动链：边界处不传递给父容器（防止整页滑出）
    lv_obj_remove_flag(scroll_container_, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_set_style_bg_opa(scroll_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_container_, 0, 0);
    lv_obj_set_style_pad_all(scroll_container_, 0, 0);
    // 内容宽 = 视口 + 最后一页起点，多一像素都不给
    lv_obj_set_content_width(scroll_container_, 820 + 410);

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
    int scroll_x = lv_obj_get_scroll_x(lv_event_get_target_obj(e));
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
    int bar_h = 34;
    status_bar_bg_ = lv_obj_create(container_);
    lv_obj_remove_style_all(status_bar_bg_);
    lv_obj_set_size(status_bar_bg_, 410, bar_h);
    lv_obj_set_pos(status_bar_bg_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_bg_, C_BG, 0);
    lv_obj_set_style_bg_opa(status_bar_bg_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_bar_bg_, 0, 0);
    lv_obj_set_style_radius(status_bar_bg_, 0, 0);

    wifi_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(wifi_label_, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(wifi_label_, C_WHITE, 0);
    lv_label_set_text(wifi_label_, font_awesome_get_utf8("wifi_slash"));
    lv_obj_align(wifi_label_, LV_ALIGN_TOP_MID, -35, 6);

    battery_icon_ = lv_label_create(container_);
    lv_obj_set_style_text_font(battery_icon_, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(battery_icon_, C_WHITE, 0);
    lv_label_set_text(battery_icon_, font_awesome_get_utf8("battery_three_quarters"));
    lv_obj_align(battery_icon_, LV_ALIGN_TOP_MID, 0, 6);

    battery_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(battery_label_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(battery_label_, C_WHITE, 0);
    lv_label_set_text(battery_label_, "--%");
    lv_obj_align(battery_label_, LV_ALIGN_TOP_MID, 35, 6);

    // 齿轮设置按钮 (44x32，大触摸区)
    settings_cog_ = lv_btn_create(container_);
    lv_obj_remove_style_all(settings_cog_);
    lv_obj_set_size(settings_cog_, 44, 32);
    lv_obj_align(settings_cog_, LV_ALIGN_TOP_MID, 78, 1);
    lv_obj_set_style_bg_opa(settings_cog_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(settings_cog_, 0, 0);
    lv_obj_set_style_border_width(settings_cog_, 0, 0);
    lv_obj_t* gear_lbl = lv_label_create(settings_cog_);
    lv_obj_set_style_text_font(gear_lbl, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(gear_lbl, C_GRAY, 0);
    lv_label_set_text(gear_lbl, font_awesome_get_utf8("gear"));
    lv_obj_center(gear_lbl);
    lv_obj_add_event_cb(settings_cog_, [](lv_event_t* e) {
        auto code = lv_event_get_code(e);
        if (code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSED)
            ESP_LOGI(TAG, "Settings btn event=%d", (int)code);
        if (code == LV_EVENT_CLICKED) {
            auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
            self->ShowSettings();
        }
    }, LV_EVENT_ALL, this);
}

// ==================== 设置面板 ====================

void WatchFace::CreateSettingsPanel() {
    // 半透明黑色遮罩 (覆盖整个屏幕)
    settings_overlay_ = lv_obj_create(container_);
    lv_obj_remove_style_all(settings_overlay_);
    lv_obj_set_size(settings_overlay_, 410, 502);
    lv_obj_set_pos(settings_overlay_, 0, 0);
    lv_obj_set_style_bg_color(settings_overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(settings_overlay_, 180, 0);
    lv_obj_set_style_border_width(settings_overlay_, 0, 0);
    lv_obj_set_style_radius(settings_overlay_, 0, 0);
    lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_CLICKABLE); // 点击遮罩关闭
    lv_obj_add_event_cb(settings_overlay_, [](lv_event_t* e) {
        auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
        self->HideSettings();
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);

    // 白色卡片 (居中)
    settings_panel_ = lv_obj_create(container_);
    lv_obj_remove_style_all(settings_panel_);
    lv_obj_set_size(settings_panel_, 340, 390);
    lv_obj_set_pos(settings_panel_, 35, 55);
    lv_obj_set_style_bg_color(settings_panel_, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(settings_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_panel_, 1, 0);
    lv_obj_set_style_border_color(settings_panel_, C_DIMMED, 0);
    lv_obj_set_style_radius(settings_panel_, 16, 0);
    lv_obj_add_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);

    int y = 10;

    // 标题栏: 返回按钮 + "设置"
    lv_obj_t* back_btn = lv_label_create(settings_panel_);
    lv_obj_set_style_text_font(back_btn, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(back_btn, C_GRAY, 0);
    lv_label_set_text(back_btn, font_awesome_get_utf8("arrow_left"));
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 15, y);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, [](lv_event_t* e) {
        auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
        self->HideSettings();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* title = lv_label_create(settings_panel_);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(title, C_WHITE, 0);
    lv_label_set_text(title, "设置");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, y);
    y += 40;

    // ---- 主题切换 ----
    lv_obj_t* lbl_theme = lv_label_create(settings_panel_);
    lv_obj_set_style_text_font(lbl_theme, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(lbl_theme, C_WHITE, 0);
    lv_label_set_text(lbl_theme, "主题");
    lv_obj_align(lbl_theme, LV_ALIGN_TOP_LEFT, 15, y);

    settings_theme_dark_ = lv_btn_create(settings_panel_);
    lv_obj_set_size(settings_theme_dark_, 80, 32);
    lv_obj_align(settings_theme_dark_, LV_ALIGN_TOP_LEFT, 120, y - 4);
    lv_obj_set_style_radius(settings_theme_dark_, 6, 0);
    lv_obj_t* lbl_dark = lv_label_create(settings_theme_dark_);
    lv_label_set_text(lbl_dark, "暗色");
    lv_obj_center(lbl_dark);

    settings_theme_light_ = lv_btn_create(settings_panel_);
    lv_obj_set_size(settings_theme_light_, 80, 32);
    lv_obj_align(settings_theme_light_, LV_ALIGN_TOP_LEFT, 210, y - 4);
    lv_obj_set_style_radius(settings_theme_light_, 6, 0);
    lv_obj_t* lbl_light = lv_label_create(settings_theme_light_);
    lv_label_set_text(lbl_light, "亮色");
    lv_obj_center(lbl_light);

    // 主题按钮回调：写 NVS + 改手表界面 + 改聊天UI
    auto theme_cb = [](lv_event_t* e) {
        auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
        auto* btn = (lv_obj_t*)lv_event_get_target(e);
        bool dark = (btn == self->settings_theme_dark_);
        std::string theme = dark ? "dark" : "light";
        Settings("display", true).SetString("theme", theme);
        self->ApplyTheme(dark);
        auto* display = Board::GetInstance().GetDisplay();
        if (display) display->SetTheme(LvglThemeManager::GetInstance().GetTheme(theme));
        self->RefreshThemeButtons();
    };
    lv_obj_add_event_cb(settings_theme_dark_, theme_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(settings_theme_light_, theme_cb, LV_EVENT_CLICKED, this);
    y += 42;

    // ---- 音量调节 ----
    lv_obj_t* lbl_vol = lv_label_create(settings_panel_);
    lv_obj_set_style_text_font(lbl_vol, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(lbl_vol, C_WHITE, 0);
    lv_label_set_text(lbl_vol, "音量");
    lv_obj_align(lbl_vol, LV_ALIGN_TOP_LEFT, 15, y);

    settings_vol_label_ = lv_label_create(settings_panel_);
    lv_obj_set_style_text_font(settings_vol_label_, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(settings_vol_label_, C_ORANGE, 0);
    lv_label_set_text(settings_vol_label_, "70%");
    lv_obj_align(settings_vol_label_, LV_ALIGN_TOP_LEFT, 290, y);

    Settings audio("audio", false);
    settings_volume_ = audio.GetInt("output_volume", 70);
    char vol_buf[8];
    snprintf(vol_buf, sizeof(vol_buf), "%d%%", settings_volume_);
    lv_label_set_text(settings_vol_label_, vol_buf);
    y += 28;

    settings_slider_ = lv_slider_create(settings_panel_);
    lv_obj_set_size(settings_slider_, 260, 30);
    lv_obj_align(settings_slider_, LV_ALIGN_TOP_LEFT, 25, y);
    lv_slider_set_range(settings_slider_, 0, 100);
    lv_slider_set_value(settings_slider_, settings_volume_, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(settings_slider_, C_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(settings_slider_, C_ORANGE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(settings_slider_, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(settings_slider_, C_WHITE, LV_PART_KNOB);
    lv_obj_set_style_pad_ver(settings_slider_, 4, LV_PART_KNOB);
    lv_obj_set_style_pad_hor(settings_slider_, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(settings_slider_, SettingsSliderCB, LV_EVENT_VALUE_CHANGED, this);
    y += 50;

    // ---- 唤醒词选择（点击展开/收起按钮组） ----
    lv_obj_t* lbl_wake = lv_label_create(settings_panel_);
    lv_obj_set_style_text_font(lbl_wake, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(lbl_wake, C_WHITE, 0);
    lv_label_set_text(lbl_wake, "唤醒词");
    lv_obj_align(lbl_wake, LV_ALIGN_TOP_LEFT, 15, y);

    y += 8;

    const char* wake_words[] = {"你好小智", "你好 大兄弟", "你好 姐妹", "嘿小智", "小智小智"};
    Settings ws("audio", false);
    std::string cur_wake = ws.GetString("wake_word", "你好小智");

    // 当前选中显示 + 展开按钮
    settings_wake_dd_ = lv_btn_create(settings_panel_);
    lv_obj_set_size(settings_wake_dd_, 200, 36);
    lv_obj_align(settings_wake_dd_, LV_ALIGN_TOP_LEFT, 115, y - 4);
    lv_obj_set_style_radius(settings_wake_dd_, 6, 0);
    lv_obj_set_style_bg_color(settings_wake_dd_, C_DARK, 0);
    lv_obj_t* wake_sel = lv_label_create(settings_wake_dd_);
    lv_label_set_text(wake_sel, cur_wake.c_str());
    lv_obj_set_style_text_font(wake_sel, &font_puhui_20_4, 0);
    lv_obj_align(wake_sel, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t* wake_arrow = lv_label_create(settings_wake_dd_);
    lv_label_set_text(wake_arrow, font_awesome_get_utf8("angle_down"));
    lv_obj_set_style_text_font(wake_arrow, &font_awesome_20_4, 0);
    lv_obj_align(wake_arrow, LV_ALIGN_RIGHT_MID, -8, 0);

    // 5个选项按钮，初始隐藏。用 file-scope static 存引用供回调访问
    static lv_obj_t* s_wake_btns[5] = {};
    static lv_obj_t* s_wake_sel = nullptr;
    s_wake_sel = wake_sel;
    static lv_obj_t* s_wake_panel = nullptr;
    for (int i = 0; i < 5; i++) {
        s_wake_btns[i] = lv_btn_create(settings_panel_);
        lv_obj_set_size(s_wake_btns[i], 200, 34);
        lv_obj_align(s_wake_btns[i], LV_ALIGN_TOP_LEFT, 115, y + 34 + i * 34);
        lv_obj_set_style_radius(s_wake_btns[i], (i == 4) ? 6 : 0, 0);
        if (cur_wake == wake_words[i]) lv_obj_set_style_bg_color(s_wake_btns[i], C_ORANGE, 0);
        lv_obj_t* lbl = lv_label_create(s_wake_btns[i]);
        lv_label_set_text(lbl, wake_words[i]);
        lv_obj_set_style_text_font(lbl, &font_puhui_16_4, 0);
        lv_obj_center(lbl);
        lv_obj_add_flag(s_wake_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_wake_btns[i], [](lv_event_t* ev) {
            auto* btn = (lv_obj_t*)lv_event_get_target(ev);
            const char* text = lv_label_get_text(lv_obj_get_child(btn, 0));
            Settings("audio", true).SetString("wake_word", text);
            lv_label_set_text(s_wake_sel, text);
            for (int j = 0; j < 5; j++) {
                bool sel = (s_wake_btns[j] == btn);
                lv_obj_set_style_bg_color(s_wake_btns[j], sel ? C_ORANGE : C_DARK, 0);
                lv_obj_add_flag(s_wake_btns[j], LV_OBJ_FLAG_HIDDEN);
            }
        }, LV_EVENT_CLICKED, nullptr);
    }
    s_wake_panel = settings_panel_;
    // 点击展开/收起
    lv_obj_add_event_cb(settings_wake_dd_, [](lv_event_t* e) {
        static bool expanded = false;
        expanded = !expanded;
        for (int j = 0; j < 5; j++) {
            if (expanded) lv_obj_remove_flag(s_wake_btns[j], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_wake_btns[j], LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_CLICKED, nullptr);
    y += 46;

    // ---- 语音模型（信息展示） ----
    lv_obj_t* lbl_model = lv_label_create(settings_panel_);
    lv_obj_set_style_text_font(lbl_model, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(lbl_model, C_WHITE, 0);
    lv_label_set_text(lbl_model, "语音模型");
    lv_obj_align(lbl_model, LV_ALIGN_TOP_LEFT, 15, y);

    settings_model_label_ = lv_label_create(settings_panel_);
    lv_obj_set_style_text_font(settings_model_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(settings_model_label_, C_GRAY, 0);
    lv_label_set_text(settings_model_label_, "云端默认 (可从小智后台切换)");
    lv_obj_align(settings_model_label_, LV_ALIGN_TOP_LEFT, 120, y + 2);
    y += 28;

    // 唤醒词提示（放语音模型下面）
    lv_obj_t* hint_wake = lv_label_create(settings_panel_);
    lv_obj_set_style_text_font(hint_wake, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(hint_wake, lv_color_hex(0xCC3333), 0);
    lv_label_set_text(hint_wake, "唤醒词需后台同步修改后生效");
    lv_obj_align(hint_wake, LV_ALIGN_TOP_LEFT, 15, y);

    // ---- 底部：关闭(大) + 重启(小) ----
    settings_save_btn_ = lv_btn_create(settings_panel_);
    lv_obj_set_size(settings_save_btn_, 200, 42);
    lv_obj_align(settings_save_btn_, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(settings_save_btn_, 10, 0);
    lv_obj_set_style_bg_color(settings_save_btn_, C_ORANGE, 0);
    lv_obj_t* lbl_save = lv_label_create(settings_save_btn_);
    lv_label_set_text(lbl_save, "关闭");
    lv_obj_set_style_text_font(lbl_save, &font_puhui_20_4, 0);
    lv_obj_center(lbl_save);
    lv_obj_add_event_cb(settings_save_btn_, [](lv_event_t* e) {
        auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
        self->HideSettings();
    }, LV_EVENT_CLICKED, this);

    // 重启按钮(小，放上面)
    lv_obj_t* reboot_btn = lv_btn_create(settings_panel_);
    lv_obj_set_size(reboot_btn, 110, 30);
    lv_obj_align(reboot_btn, LV_ALIGN_BOTTOM_MID, 0, -66);
    lv_obj_set_style_radius(reboot_btn, 10, 0);
    lv_obj_set_style_bg_color(reboot_btn, lv_color_hex(0xCC3333), 0);
    lv_obj_t* lbl_reboot = lv_label_create(reboot_btn);
    lv_label_set_text(lbl_reboot, "重启设备");
    lv_obj_set_style_text_font(lbl_reboot, &font_puhui_20_4, 0);
    lv_obj_center(lbl_reboot);
    lv_obj_add_event_cb(reboot_btn, [](lv_event_t* e) {
        auto& app = Application::GetInstance();
        app.Schedule([&app]() { vTaskDelay(pdMS_TO_TICKS(500)); app.Reboot(); });
    }, LV_EVENT_CLICKED, nullptr);

    RefreshThemeButtons();
}

void WatchFace::RefreshThemeButtons() {
    Settings s("display", false);
    std::string theme = s.GetString("theme", "dark");
    bool is_dark = (theme == "dark");
    lv_obj_set_style_border_width(settings_theme_dark_, is_dark ? 2 : 0, 0);
    lv_obj_set_style_border_color(settings_theme_dark_, C_ORANGE, 0);
    lv_obj_set_style_border_width(settings_theme_light_, is_dark ? 0 : 2, 0);
    lv_obj_set_style_border_color(settings_theme_light_, C_ORANGE, 0);
}

void WatchFace::ApplyTheme(bool dark) {
    lv_color_t bg     = dark ? C_BG    : lv_color_hex(0xFFFFFF);
    lv_color_t fg     = dark ? C_WHITE : lv_color_hex(0x000000);
    lv_color_t gray   = dark ? C_GRAY  : lv_color_hex(0x666666);
    lv_color_t dimmed = dark ? C_DIMMED: lv_color_hex(0x999999);
    lv_color_t darker = dark ? C_DARKER: lv_color_hex(0xBBBBBB);
    lv_color_t dark_bg= dark ? C_DARK  : lv_color_hex(0xDDDDDD);

    auto set_bg = [](lv_obj_t* o, lv_color_t c, int p = 0) { lv_obj_set_style_bg_color(o, c, p); };
    auto set_clr= [](lv_obj_t* o, lv_color_t c) { lv_obj_set_style_text_color(o, c, 0); };
    auto set_line = [](lv_obj_t* o, lv_color_t c) { lv_obj_set_style_line_color(o, c, 0); };
    auto set_arc = [](lv_obj_t* o, lv_color_t c, int p) { lv_obj_set_style_arc_color(o, c, p); };
    auto set_border = [](lv_obj_t* o, lv_color_t c) { lv_obj_set_style_border_color(o, c, 0); };

    // 根容器 + 状态栏
    set_bg(container_, bg);
    set_bg(status_bar_bg_, bg);

    // 状态栏图标/文字
    set_clr(wifi_label_, fg);
    set_clr(battery_icon_, fg);
    set_clr(battery_label_, fg);
    set_clr(lv_obj_get_child(settings_cog_, 0), gray); // gear label inside btn

    // 主页
    set_clr(clock_label_, fg);
    set_clr(date_label_, C_ORANGE);  // 日期始终橙色
    set_clr(weather_icon_, fg);
    set_clr(weather_label_, gray);
    set_clr(tap_hint_[0], gray);

    // 步数页
    set_bg(steps_arc_, dark_bg, LV_PART_MAIN);
    set_arc(steps_arc_, C_ORANGE, LV_PART_INDICATOR);
    set_clr(steps_count_label_, fg);
    set_clr(steps_target_label_, gray);
    set_clr(tap_hint_[1], gray);

    // 指南针
    set_border(compass_ring_outer_, dimmed);
    set_border(compass_ring_inner_, darker);
    for (int i = 0; i < 16; i++) {
        lv_color_t tc;
        if (i % 4 == 0) tc = fg;
        else if (i % 2 == 0) tc = dimmed;
        else tc = darker;
        set_line(compass_tick_[i], tc);
    }
    set_clr(compass_lbl_n_, C_ORANGE);
    set_clr(compass_lbl_s_, gray);
    set_clr(compass_lbl_e_, gray);
    set_clr(compass_lbl_w_, gray);
    set_bg(compass_pointer_, fg);
    set_bg(compass_pointer_south_, C_ORANGE);
    set_bg(compass_dot_, C_ORANGE);
    set_clr(compass_heading_label_, fg);
    set_clr(tap_hint_[2], gray);

    // 页面指示器（未激活）
    for (int i = 0; i < 3; i++) {
        if (i != current_page_) set_bg(page_dot_[i], darker);
    }

    // 强制刷新
    lv_obj_invalidate(container_);
}

void WatchFace::ShowSettings() {
    lv_obj_set_scroll_dir(scroll_container_, LV_DIR_NONE);
    // 刷新音量滑动条（物理按键可能已改变音量）
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        settings_volume_ = codec->output_volume();
        lv_slider_set_value(settings_slider_, settings_volume_, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", settings_volume_);
        lv_label_set_text(settings_vol_label_, buf);
    }

    lv_obj_remove_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(settings_overlay_);
    lv_obj_move_foreground(settings_panel_);
}

void WatchFace::HideSettings() {
    lv_obj_set_scroll_dir(scroll_container_, LV_DIR_HOR);
    lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);
}

// ==================== 音量 Toast ====================
static lv_obj_t* s_vol_toast = nullptr;
static lv_timer_t* s_vol_timer = nullptr;

static void vol_toast_hide(lv_timer_t*) {
    if (s_vol_toast) { lv_obj_add_flag(s_vol_toast, LV_OBJ_FLAG_HIDDEN); s_vol_toast = nullptr; }
    if (s_vol_timer) { lv_timer_del(s_vol_timer); s_vol_timer = nullptr; }
}

void WatchFace::ShowVolumeToast(int vol) {
    if (!s_vol_toast) {
        s_vol_toast = lv_label_create(container_);
        lv_obj_set_style_text_font(s_vol_toast, &font_puhui_30_4, 0);
        lv_obj_set_style_text_color(s_vol_toast, C_WHITE, 0);
        lv_obj_set_style_bg_color(s_vol_toast, lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_opa(s_vol_toast, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_vol_toast, 10, 0);
        lv_obj_set_style_pad_all(s_vol_toast, 12, 0);
        lv_obj_add_flag(s_vol_toast, LV_OBJ_FLAG_HIDDEN);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "🔊 %d%%", vol);
    lv_label_set_text(s_vol_toast, buf);
    lv_obj_align(s_vol_toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_vol_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_vol_toast);
    // 1.5s 后自动隐藏
    if (s_vol_timer) lv_timer_del(s_vol_timer);
    s_vol_timer = lv_timer_create(vol_toast_hide, 1500, nullptr);
    lv_timer_set_repeat_count(s_vol_timer, 1);
}

void WatchFace::SettingsSliderCB(lv_event_t* e) {
    auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
    int vol = (int)lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
    self->settings_volume_ = vol;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", vol);
    lv_label_set_text(self->settings_vol_label_, buf);
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec) codec->SetOutputVolume(vol);
}

void WatchFace::UpdateStatusBar() {
    bool wifi_ok = false;
    auto* app = &Application::GetInstance();
    if (app) {
        auto state = app->GetDeviceState();
        wifi_ok = (state == kDeviceStateIdle || state == kDeviceStateListening ||
                   state == kDeviceStateSpeaking || state == kDeviceStateConnecting);
    }
    Settings s("display", false);
    bool dark = (s.GetString("theme", "dark") == "dark");
    lv_color_t fg = dark ? C_WHITE : lv_color_hex(0x000000);
    lv_color_t dim = dark ? lv_color_hex(0x666666) : lv_color_hex(0x999999);

    lv_label_set_text(wifi_label_, font_awesome_get_utf8(wifi_ok ? "wifi" : "wifi_slash"));
    lv_obj_set_style_text_color(wifi_label_, wifi_ok ? fg : dim, 0);

    lv_label_set_text(battery_icon_, font_awesome_get_utf8("battery_full"));
    lv_obj_set_style_text_color(battery_icon_, fg, 0);
    lv_obj_set_style_text_color(battery_label_, fg, 0);
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
    char lower[64] = {};
    for (int i = 0; desc[i] && i < 63; i++) lower[i] = tolower(desc[i]);

    if (strstr(lower,"thunder")) return "cloud_bolt";
    if (strstr(lower,"rain")||strstr(lower,"drizzle")||strstr(lower,"shower")||strstr(lower,"patchy")) return "cloud_rain";
    if (strstr(lower,"snow")||strstr(lower,"ice")) return "snowflake";
    if (strstr(lower,"mist")||strstr(lower,"fog")||strstr(lower,"haze")) return "smog";
    if (strstr(lower,"overcast")) return "clouds";
    if (strstr(lower,"cloud")||strstr(lower,"partly")) return "cloud_sun";
    if (strstr(lower,"sunny")||strstr(lower,"clear")) return "sun";
    return "sun";
}

void WatchFace::UpdateWeather(const char* desc, int temp_c) {
    const char* fa_icon = WeatherIcon(desc);
    const char* fa_utf8 = font_awesome_get_utf8(fa_icon);
    if (fa_utf8 && fa_utf8[0]) {
        lv_label_set_text(weather_icon_, fa_utf8);
    }

    // 中文翻译（大小写不敏感）
    const char* cn_desc = desc ? desc : "";
    // 转为小写副本用于匹配
    char lower[64] = {};
    if (desc) { for (int i = 0; desc[i] && i < 63; i++) lower[i] = tolower(desc[i]); }

    if      (strstr(lower,"thunder"))                       cn_desc = "雷雨";
    else if (strstr(lower,"snow")||strstr(lower,"ice"))     cn_desc = "雪";
    else if (strstr(lower,"rain")||strstr(lower,"drizzle")||strstr(lower,"shower")||strstr(lower,"patchy")) cn_desc = "雨";
    else if (strstr(lower,"overcast"))                      cn_desc = "阴";
    else if (strstr(lower,"cloud")||strstr(lower,"partly")) cn_desc = "多云";
    else if (strstr(lower,"sunny")||strstr(lower,"clear"))  cn_desc = "晴";
    else if (strstr(lower,"mist")||strstr(lower,"fog")||strstr(lower,"haze")) cn_desc = "雾";

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
