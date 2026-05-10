#ifndef _WATCH_FACE_H_
#define _WATCH_FACE_H_

#include <lvgl.h>

class WatchFace {
public:
    WatchFace(lv_obj_t* parent);
    ~WatchFace();

    void Show();
    void Hide();
    lv_obj_t* GetTapArea()  { return tap_area_; }

    void UpdateClock();
    void UpdateWeather(const char* desc, int temp_c);
    void UpdateSteps(int steps);
    void UpdateCompass(float heading_deg);

private:
    lv_obj_t* container_;
    lv_obj_t* clock_label_;
    lv_obj_t* date_label_;
    lv_obj_t* weather_icon_;
    lv_obj_t* weather_label_;
    lv_obj_t* steps_label_;
    lv_obj_t* tap_area_;
    lv_obj_t* tap_hint_;

    // 指南针 — 纯 LVGL 对象
    lv_obj_t* compass_cont_;
    lv_obj_t* compass_ring_;
    lv_obj_t* compass_dot_;
    lv_obj_t* compass_pointer_;
    lv_point_precise_t compass_points_[2];
    lv_obj_t* compass_lbl_n_;
    lv_obj_t* compass_lbl_s_;
    lv_obj_t* compass_lbl_e_;
    lv_obj_t* compass_lbl_w_;

    // 状态栏
    lv_obj_t* wifi_label_;
    lv_obj_t* battery_icon_;
    lv_obj_t* battery_label_;

    lv_timer_t* clock_timer_;
    lv_timer_t* status_timer_;
    float compass_heading_;

    void CreateCompass(int cx, int cy, int radius);
    void UpdateCompassPointer();
    void CreateStatusBar();
    void UpdateStatusBar();
    static void ClockTimerCB(lv_timer_t* timer);
    static void StatusTimerCB(lv_timer_t* timer);
    static const char* WeatherIcon(const char* desc);
};

#endif
