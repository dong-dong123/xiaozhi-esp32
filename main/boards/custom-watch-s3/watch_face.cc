#include "watch_face.h"
#include <esp_log.h>
#include <cstdio>
#include <cmath>

LV_FONT_DECLARE(font_puhui_basic_30_4);
LV_FONT_DECLARE(font_puhui_basic_20_4);

#define TAG "WatchFace"

static const lv_color_t C_BG    = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t C_WHITE = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t C_GRAY  = LV_COLOR_MAKE(0x99, 0x99, 0x99);
static const lv_color_t C_ORANGE= LV_COLOR_MAKE(0xFF, 0x95, 0x00);

WatchFace::WatchFace(lv_obj_t* parent)
    : compass_heading_(0)
{
    const lv_font_t* font_small = &font_puhui_basic_20_4;

    // 全屏容器——LVGL 最顶层
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

    // ===== 大时钟 — 30px =====
    clock_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(clock_label_, &font_puhui_basic_30_4, 0);
    lv_obj_set_style_text_color(clock_label_, C_WHITE, 0);
    lv_label_set_text(clock_label_, "--:--");
    lv_obj_align(clock_label_, LV_ALIGN_TOP_MID, 0, 50);

    // ===== 日期 =====
    date_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(date_label_, font_small, 0);
    lv_obj_set_style_text_color(date_label_, C_ORANGE, 0);
    lv_label_set_text(date_label_, "5/9 Fri");
    lv_obj_align(date_label_, LV_ALIGN_TOP_MID, 0, 115);

    // ===== 天气行 =====
    weather_label_ = lv_label_create(container_);
    lv_obj_set_style_text_font(weather_label_, font_small, 0);
    lv_obj_set_style_text_color(weather_label_, C_GRAY, 0);
    lv_label_set_text(weather_label_, "");
    lv_obj_align(weather_label_, LV_ALIGN_TOP_MID, 0, 145);

    // ===== 指南针 100x100 =====
    compass_canvas_ = lv_canvas_create(container_);
    lv_obj_set_size(compass_canvas_, 100, 100);
    lv_obj_align(compass_canvas_, LV_ALIGN_CENTER, 0, 60);

    compass_draw_buf_ = (lv_draw_buf_t*)malloc(
        sizeof(lv_draw_buf_t) + 100 * 100 * sizeof(lv_color_t));
    if (compass_draw_buf_) {
        memset(compass_draw_buf_, 0, sizeof(lv_draw_buf_t) + 100 * 100 * sizeof(lv_color_t));
        compass_draw_buf_->header.w = 100;
        compass_draw_buf_->header.h = 100;
        compass_draw_buf_->header.cf = LV_COLOR_FORMAT_RGB565;
        compass_draw_buf_->header.stride = 100 * sizeof(lv_color_t);
        compass_draw_buf_->data_size = 100 * 100 * sizeof(lv_color_t);
        compass_draw_buf_->data = (uint8_t*)(compass_draw_buf_ + 1);
        compass_draw_buf_->unaligned_data = compass_draw_buf_->data;
        lv_canvas_set_draw_buf(compass_canvas_, compass_draw_buf_);
    }
    DrawCompass();

    // ===== 点击区域 =====
    tap_area_ = lv_obj_create(container_);
    lv_obj_set_size(tap_area_, 410, 50);
    lv_obj_align(tap_area_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tap_area_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap_area_, 0, 0);

    tap_hint_ = lv_label_create(container_);
    lv_obj_set_style_text_font(tap_hint_, font_small, 0);
    lv_obj_set_style_text_color(tap_hint_, C_GRAY, 0);
    lv_label_set_text(tap_hint_, "Tap to wake");
    lv_obj_align(tap_hint_, LV_ALIGN_BOTTOM_MID, 0, -10);

    // 1秒刷新时钟
    clock_timer_ = lv_timer_create(ClockTimerCB, 1000, this);
    UpdateClock();

    ESP_LOGI(TAG, "WatchFace created");
    // 诊断日志：打印所有元素的实际位置和可见状态
    ESP_LOGI(TAG, "DIAG: container=%p vis=%d pos=%d,%d size=%dx%d parent=%dx%d",
             container_, !lv_obj_has_flag(container_, LV_OBJ_FLAG_HIDDEN),
             lv_obj_get_x(container_), lv_obj_get_y(container_),
             lv_obj_get_width(container_), lv_obj_get_height(container_),
             lv_obj_get_width(parent), lv_obj_get_height(parent));
    auto log_label = [](const char* name, lv_obj_t* obj) {
        if (!obj) return;
        ESP_LOGI(TAG, "DIAG: %s=%p vis=%d pos=%d,%d size=%dx%d",
                 name, obj, !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN),
                 lv_obj_get_x(obj), lv_obj_get_y(obj),
                 lv_obj_get_width(obj), lv_obj_get_height(obj));
    };
    log_label("clock", clock_label_);
    log_label("date", date_label_);
    log_label("weather", weather_label_);
    log_label("compass", compass_canvas_);
    log_label("tap", tap_hint_);
    ESP_LOGI(TAG, "DIAG: layer_top=%p scr_act=%p", lv_layer_top(), lv_scr_act());

    // 延迟诊断：等 LVGL 渲染首帧后再检查实际尺寸
    lv_timer_create([](lv_timer_t* t) {
        auto* wf = static_cast<WatchFace*>(lv_timer_get_user_data(t));
        lv_timer_del(t);
        ESP_LOGI(TAG, "DIAG2: ===== After LVGL render =====");
        ESP_LOGI(TAG, "DIAG2: container=%p vis=%d pos=%d,%d size=%dx%d",
                 wf->container_, !lv_obj_has_flag(wf->container_, LV_OBJ_FLAG_HIDDEN),
                 lv_obj_get_x(wf->container_), lv_obj_get_y(wf->container_),
                 lv_obj_get_width(wf->container_), lv_obj_get_height(wf->container_));
        ESP_LOGI(TAG, "DIAG2: clock=%p vis=%d pos=%d,%d size=%dx%d",
                 wf->clock_label_, !lv_obj_has_flag(wf->clock_label_, LV_OBJ_FLAG_HIDDEN),
                 lv_obj_get_x(wf->clock_label_), lv_obj_get_y(wf->clock_label_),
                 lv_obj_get_width(wf->clock_label_), lv_obj_get_height(wf->clock_label_));
        ESP_LOGI(TAG, "DIAG2: date=%p vis=%d pos=%d,%d size=%dx%d",
                 wf->date_label_, !lv_obj_has_flag(wf->date_label_, LV_OBJ_FLAG_HIDDEN),
                 lv_obj_get_x(wf->date_label_), lv_obj_get_y(wf->date_label_),
                 lv_obj_get_width(wf->date_label_), lv_obj_get_height(wf->date_label_));
        ESP_LOGI(TAG, "DIAG2: weather=%p vis=%d pos=%d,%d size=%dx%d",
                 wf->weather_label_, !lv_obj_has_flag(wf->weather_label_, LV_OBJ_FLAG_HIDDEN),
                 lv_obj_get_x(wf->weather_label_), lv_obj_get_y(wf->weather_label_),
                 lv_obj_get_width(wf->weather_label_), lv_obj_get_height(wf->weather_label_));
        ESP_LOGI(TAG, "DIAG2: compass=%p vis=%d pos=%d,%d size=%dx%d",
                 wf->compass_canvas_, !lv_obj_has_flag(wf->compass_canvas_, LV_OBJ_FLAG_HIDDEN),
                 lv_obj_get_x(wf->compass_canvas_), lv_obj_get_y(wf->compass_canvas_),
                 lv_obj_get_width(wf->compass_canvas_), lv_obj_get_height(wf->compass_canvas_));
        ESP_LOGI(TAG, "DIAG2: tap=%p vis=%d pos=%d,%d size=%dx%d",
                 wf->tap_hint_, !lv_obj_has_flag(wf->tap_hint_, LV_OBJ_FLAG_HIDDEN),
                 lv_obj_get_x(wf->tap_hint_), lv_obj_get_y(wf->tap_hint_),
                 lv_obj_get_width(wf->tap_hint_), lv_obj_get_height(wf->tap_hint_));
    }, 1500, this);
}

WatchFace::~WatchFace() {
    if (clock_timer_) lv_timer_delete(clock_timer_);
    if (compass_draw_buf_) free(compass_draw_buf_);
    if (container_) lv_obj_delete(container_);
}

void WatchFace::Show() {
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
}

void WatchFace::Hide() {
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
}

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

    const char* wdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    snprintf(buf, sizeof(buf), "%d/%d %s", tm->tm_mon + 1, tm->tm_mday, wdays[tm->tm_wday]);
    lv_label_set_text(date_label_, buf);
}

void WatchFace::UpdateWeather(const char* desc, int temp_c) {
    if (!desc || desc[0] == '\0') return;
    char buf[128];
    const char* icon = "";
    if      (strstr(desc, "Sun")||strstr(desc,"晴")) icon = "☀";
    else if (strstr(desc, "Cloud")||strstr(desc,"云")) icon = "☁";
    else if (strstr(desc, "Rain")||strstr(desc,"雨")) icon = "🌧";
    else if (strstr(desc, "Snow")||strstr(desc,"雪")) icon = "❄";
    else if (strstr(desc, "Mist")||strstr(desc,"Fog")||strstr(desc,"雾")) icon = "🌫";
    else if (strstr(desc, "Thunder")||strstr(desc,"雷")) icon = "⚡";
    snprintf(buf, sizeof(buf), "%s  %d°C  %s", icon, temp_c, desc);
    lv_label_set_text(weather_label_, buf);
}

void WatchFace::UpdateSteps(int) {}

void WatchFace::DrawCompass() {
    // TODO: 等 UI 重构后用 LVGL 对象替代 canvas
}

    lv_layer_t layer;
    lv_canvas_init_layer(compass_canvas_, &layer);

    // 外圈
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.color = lv_color_hex(0x666666);
    arc_dsc.width = 2;
    arc_dsc.radius = 48;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 3600;
    lv_draw_arc(&layer, &arc_dsc);

    // 刻度线
    for (int i = 0; i < 360; i += 30) {
        float rad = (i - 90) * M_PI / 180.0f;
        int inner = (i % 90 == 0) ? 34 : 38;
        lv_point_precise_t p1 = {50 + (int)(inner * cosf(rad)), 50 + (int)(inner * sinf(rad))};
        lv_point_precise_t p2 = {50 + (int)(46 * cosf(rad)), 50 + (int)(46 * sinf(rad))};
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = (i % 90 == 0) ? C_WHITE : lv_color_hex(0x888888);
        line_dsc.width = (i % 90 == 0) ? 2 : 1;
        line_dsc.p1 = p1; line_dsc.p2 = p2;
        lv_draw_line(&layer, &line_dsc);
    }

    // 指针
    float rad = (compass_heading_ - 90) * M_PI / 180.0f;
    lv_point_precise_t tip = {50 + (int)(42 * cosf(rad)), 50 + (int)(42 * sinf(rad))};
    lv_point_precise_t base = {50 - (int)(42 * cosf(rad)), 50 - (int)(42 * sinf(rad))};
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = C_ORANGE;
    line_dsc.width = 3;
    line_dsc.p1 = base; line_dsc.p2 = tip;
    lv_draw_line(&layer, &line_dsc);

    // N标记
    lv_area_t n_area = {42, 0, 58, 18};
    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = C_ORANGE;
    lbl.font = &font_puhui_basic_20_4;
    lbl.text = "N";
    lv_draw_label(&layer, &lbl, &n_area);

    lv_canvas_finish_layer(compass_canvas_, &layer);
    lv_obj_invalidate(compass_canvas_);
}

void WatchFace::UpdateCompass(float heading_deg) {
    compass_heading_ = heading_deg;
    DrawCompass();
}
