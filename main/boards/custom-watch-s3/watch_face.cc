#include "watch_face.h"
#include <esp_log.h>
#include <cstdio>
#include <cmath>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

#define TAG "WatchFace"

// AMOLED 深色配色
#define COLOR_BG       lv_color_hex(0x000000)
#define COLOR_PRIMARY  lv_color_hex(0xFFFFFF)
#define COLOR_SECOND   lv_color_hex(0x888888)
#define COLOR_ACCENT   lv_color_hex(0xFF9500)

// ==================== 构造 ====================

WatchFace::WatchFace(lv_obj_t* parent)
    : compass_heading_(0)
{
    // 容器（叠加在 LcdDisplay 屏幕上）
    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_remove_style_all(container_);
    lv_obj_set_style_bg_color(container_, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_center(container_);

    // 项目字体（Puhui 20px + 14px 小字）
    const lv_font_t* font_big = &BUILTIN_TEXT_FONT;
    lv_obj_set_style_text_color(container_, COLOR_PRIMARY, 0);

    // ====== 大数字时钟 (居中偏上) ======
    clock_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(clock_label_, font_big, 0);
    lv_obj_set_style_text_color(clock_label_, COLOR_PRIMARY, 0);
    lv_label_set_text(clock_label_, "--:--");
    lv_obj_align(clock_label_, LV_ALIGN_CENTER, 0, -70);

    // ====== 日期 ======
    date_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(date_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(date_label_, COLOR_ACCENT, 0);
    lv_label_set_text(date_label_, "----/--/-- ---");
    lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, -15);

    // ====== 天气行 ======
    weather_icon_ = lv_label_create(container_);
    lv_obj_set_style_text_font(weather_icon_, font_big, 0);
    lv_label_set_text(weather_icon_, "");
    lv_obj_align(weather_icon_, LV_ALIGN_CENTER, -70, 30);

    weather_temp_ = lv_label_create(container_);
    lv_obj_set_style_text_font(weather_temp_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(weather_temp_, COLOR_PRIMARY, 0);
    lv_label_set_text(weather_temp_, "");
    lv_obj_align_to(weather_temp_, weather_icon_, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    weather_desc_ = lv_label_create(container_);
    lv_obj_set_style_text_font(weather_desc_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(weather_desc_, COLOR_SECOND, 0);
    lv_label_set_text(weather_desc_, "");
    lv_obj_align_to(weather_desc_, weather_temp_, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    // ====== 指南针画布 (120x120, 居中偏下) ======
    compass_canvas_ = lv_canvas_create(container_);
    lv_obj_set_size(compass_canvas_, 120, 120);
    lv_obj_align(compass_canvas_, LV_ALIGN_CENTER, 0, 120);

    compass_draw_buf_ = (lv_draw_buf_t*)malloc(
        sizeof(lv_draw_buf_t) + 120 * 120 * sizeof(lv_color_t));
    if (compass_draw_buf_) {
        memset(compass_draw_buf_, 0, sizeof(lv_draw_buf_t) + 120 * 120 * sizeof(lv_color_t));
        compass_draw_buf_->header.w = 120;
        compass_draw_buf_->header.h = 120;
        compass_draw_buf_->header.cf = LV_COLOR_FORMAT_RGB565;
        compass_draw_buf_->header.stride = 120;
        compass_draw_buf_->data_size = 120 * 120 * sizeof(lv_color_t);
        compass_draw_buf_->data = (uint8_t*)(compass_draw_buf_ + 1);
        compass_draw_buf_->unaligned_data = compass_draw_buf_->data;
        lv_canvas_set_draw_buf(compass_canvas_, compass_draw_buf_);
    }
    DrawCompass();

    // ====== 步数 ======
    steps_icon_ = lv_label_create(container_);
    lv_obj_set_style_text_font(steps_icon_, &lv_font_montserrat_14, 0);
    lv_label_set_text(steps_icon_, "");
    lv_obj_align(steps_icon_, LV_ALIGN_BOTTOM_LEFT, 10, -45);

    steps_count_ = lv_label_create(container_);
    lv_obj_set_style_text_font(steps_count_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(steps_count_, COLOR_SECOND, 0);
    lv_label_set_text(steps_count_, "");
    lv_obj_align_to(steps_count_, steps_icon_, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    // ====== 底部点击区域 + 提示 ======
    tap_area_ = lv_obj_create(container_);
    lv_obj_set_size(tap_area_, LV_HOR_RES, 44);
    lv_obj_align(tap_area_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tap_area_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap_area_, 0, 0);

    tap_hint_ = lv_label_create(container_);
    lv_obj_set_style_text_font(tap_hint_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tap_hint_, COLOR_SECOND, 0);
    lv_label_set_text(tap_hint_, "Tap to wake");
    lv_obj_align(tap_hint_, LV_ALIGN_BOTTOM_MID, 0, -5);

    // ====== 1秒时钟定时器 ======
    clock_timer_ = lv_timer_create(ClockTimerCB, 1000, this);

    ESP_LOGI(TAG, "WatchFace created (project fonts, LVGL 9.x compat)");
}

WatchFace::~WatchFace() {
    if (clock_timer_) lv_timer_delete(clock_timer_);
    if (compass_draw_buf_) free(compass_draw_buf_);
    if (container_) lv_obj_delete(container_);
}

// ==================== 显隐控制 ====================

void WatchFace::Show() {
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
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

    if (tm->tm_year + 1900 >= 2025) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
        lv_label_set_text(clock_label_, buf);

        snprintf(buf, sizeof(buf), "%s %s %d",
            (const char*[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[tm->tm_wday],
            (const char*[]){"Jan","Feb","Mar","Apr","May","Jun",
                           "Jul","Aug","Sep","Oct","Nov","Dec"}[tm->tm_mon],
            tm->tm_mday);
        lv_label_set_text(date_label_, buf);
    }
}

// ==================== 天气 ====================

void WatchFace::UpdateWeather(const char* desc, int temp_c) {
    if (!desc || desc[0] == '\0') return;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d C", temp_c);
    lv_label_set_text(weather_temp_, buf);
    lv_label_set_text(weather_desc_, desc);

    // Unicode 天气符号
    const char* icon = "";
    if (strstr(desc, "Sunny") || strstr(desc, "Clear")) icon = "*";
    else if (strstr(desc, "Cloud")) icon = "~";
    else if (strstr(desc, "Rain") || strstr(desc, "Drizzle") || strstr(desc, "Shower")) icon = ".";
    else if (strstr(desc, "Snow")) icon = ",";
    else if (strstr(desc, "Mist") || strstr(desc, "Fog")) icon = "=";
    else if (strstr(desc, "Thunder")) icon = "!";
    else icon = "o";

    lv_label_set_text(weather_icon_, icon);
}

// ==================== 步数 ====================

void WatchFace::UpdateSteps(int steps) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Steps:%d", steps);
    lv_label_set_text(steps_count_, buf);
    lv_label_set_text(steps_icon_, ">");  // 简易脚步图标
}

// ==================== 指南针 ====================

void WatchFace::DrawCompass() {
    if (!compass_canvas_ || !compass_draw_buf_) return;

    static bool first = true;
    if (first) {
        first = false;
        lv_canvas_fill_bg(compass_canvas_, COLOR_BG, LV_OPA_COVER);

        lv_layer_t layer;
        lv_canvas_init_layer(compass_canvas_, &layer);

        // 外圈
        lv_draw_arc_dsc_t arc_dsc;
        lv_draw_arc_dsc_init(&arc_dsc);
        arc_dsc.color = lv_color_hex(0x333333);
        arc_dsc.width = 2;
        arc_dsc.radius = 58;
        arc_dsc.start_angle = 0;
        arc_dsc.end_angle = 3600;
        lv_draw_arc(&layer, &arc_dsc);

        // N/S/E/W 刻度线 (LVGL 9.x 用不同方式画线)
        for (int i = 0; i < 360; i += 30) {
            float rad = (i - 90) * M_PI / 180.0f;
            int inner = (i % 90 == 0) ? 42 : 47;
            lv_point_precise_t p1 = {
                (lv_value_precise_t)(60 + inner * cosf(rad)),
                (lv_value_precise_t)(60 + inner * sinf(rad))
            };
            lv_point_precise_t p2 = {
                (lv_value_precise_t)(60 + 56 * cosf(rad)),
                (lv_value_precise_t)(60 + 56 * sinf(rad))
            };
            lv_draw_line_dsc_t line_dsc;
            lv_draw_line_dsc_init(&line_dsc);
            line_dsc.color = (i % 90 == 0) ? COLOR_PRIMARY : lv_color_hex(0x444444);
            line_dsc.width = (i % 90 == 0) ? 2 : 1;
            line_dsc.p1 = p1;
            line_dsc.p2 = p2;
            lv_draw_line(&layer, &line_dsc);
        }

        lv_canvas_finish_layer(compass_canvas_, &layer);
    }

    // 旋转指针
    int16_t angle = (int16_t)(compass_heading_ * 10.0f);
    lv_obj_set_style_transform_rotation(compass_canvas_, angle, 0);
}

void WatchFace::UpdateCompass(float heading_deg) {
    compass_heading_ = heading_deg;
    DrawCompass();
}
