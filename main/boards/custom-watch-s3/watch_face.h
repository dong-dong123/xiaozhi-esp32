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
    lv_obj_t* clock_label_;
    lv_obj_t* date_label_;
    lv_obj_t* weather_label_;
    lv_obj_t* compass_canvas_;
    lv_draw_buf_t* compass_draw_buf_;
    float compass_heading_;
    lv_obj_t* tap_area_;
    lv_obj_t* tap_hint_;
    lv_timer_t* clock_timer_;

    static void ClockTimerCB(lv_timer_t* timer);
    void DrawCompass();
};

#endif
