#ifndef _WATCH_FACE_H_
#define _WATCH_FACE_H_

#include <lvgl.h>
#include <ctime>

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

    // 时钟
    lv_obj_t* clock_label_;
    lv_obj_t* date_label_;

    // 天气
    lv_obj_t* weather_icon_;
    lv_obj_t* weather_temp_;
    lv_obj_t* weather_desc_;

    // 指南针
    lv_obj_t* compass_canvas_;
    lv_draw_buf_t* compass_draw_buf_;
    float compass_heading_;

    // 步数
    lv_obj_t* steps_icon_;
    lv_obj_t* steps_count_;

    // 交互
    lv_obj_t* tap_area_;
    lv_obj_t* tap_hint_;

    lv_timer_t* clock_timer_;

    // 时钟定时器回调
    static void ClockTimerCB(lv_timer_t* timer);
    // 指南针重绘
    void DrawCompass();
    static void CompassDrawCB(lv_event_t* e);

    // 星期转换
    const char* WeekdayName(int wday);
};

#endif
