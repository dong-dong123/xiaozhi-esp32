#ifndef _WATCH_FACE_H_
#define _WATCH_FACE_H_

#include <lvgl.h>

class WatchFace {
public:
    WatchFace(lv_obj_t* parent);
    ~WatchFace();

    void Show();
    void Hide();
    lv_obj_t* GetTapArea()      { return tap_area_[0]; }
    lv_obj_t* GetTapArea(int p) { return (p >= 0 && p < 3) ? tap_area_[p] : tap_area_[0]; }

    void UpdateClock();
    void UpdateWeather(const char* desc, int temp_c);
    void UpdateSteps(int steps);
    void UpdateCompass(float heading_deg);
    void ShowVolumeToast(int vol);

private:
    lv_obj_t* container_;           // 根容器 410x502
    lv_obj_t* scroll_container_;    // 滚动视口
    lv_obj_t* page_[3];             // 三个页面容器
    lv_obj_t* page_dot_[3];         // 页面指示器圆点
    int current_page_;

    // 状态栏 (overlay)
    lv_obj_t* status_bar_bg_;
    lv_obj_t* wifi_label_;
    lv_obj_t* battery_icon_;
    lv_obj_t* battery_label_;

    // Page 0: 主页
    lv_obj_t* clock_label_;
    lv_obj_t* date_label_;
    lv_obj_t* weather_icon_;
    lv_obj_t* weather_label_;

    // Page 1: 步数
    lv_obj_t* steps_arc_;
    lv_obj_t* steps_count_label_;
    lv_obj_t* steps_target_label_;

    // Page 2: 指南针
    lv_obj_t* compass_ring_outer_;
    lv_obj_t* compass_ring_inner_;
    lv_obj_t* compass_tick_[16];
    lv_point_precise_t compass_tick_pts_[16][2];
    lv_obj_t* compass_pointer_;         // 北向杆
    lv_obj_t* compass_pointer_south_;   // 南向杆
    lv_obj_t* compass_dot_;           // 中心点
    lv_obj_t* compass_lbl_n_, *compass_lbl_s_, *compass_lbl_e_, *compass_lbl_w_;
    lv_obj_t* compass_heading_label_;
    float compass_heading_;

    // 底部按钮 (每页各一个)
    lv_obj_t* tap_area_[3];
    lv_obj_t* tap_hint_[3];

    // 定时器
    lv_timer_t* clock_timer_;
    lv_timer_t* status_timer_;

    // 设置面板
    lv_obj_t* settings_cog_;          // 状态栏齿轮图标
    lv_obj_t* settings_overlay_;      // 半透明遮罩
    lv_obj_t* settings_panel_;        // 居中卡片
    lv_obj_t* settings_slider_;       // 音量滑动条
    lv_obj_t* settings_vol_label_;    // 音量数值
    lv_obj_t* settings_theme_dark_;   // 暗色按钮
    lv_obj_t* settings_theme_light_;  // 亮色按钮
    lv_obj_t* settings_wake_dd_;      // 唤醒词下拉框
    lv_obj_t* settings_model_label_;  // 模型信息展示
    lv_obj_t* settings_save_btn_;     // 保存并重启按钮
    int settings_volume_;

    // 私有方法
    void CreateScrollLayout();
    void CreatePageIndicator();
    void CreateHomePage();
    void CreateStepsPage();
    void CreateCompassPage();
    void CreateStatusBar();
    void CreateSettingsPanel();
    void UpdateStatusBar();
    void UpdateCompassPointer();
    void UpdatePageIndicator();
    void RefreshThemeButtons();
    void ApplyTheme(bool dark);
    void ShowSettings();
    void HideSettings();
    static void ScrollEventCB(lv_event_t* e);
    static void ClockTimerCB(lv_timer_t* timer);
    static void StatusTimerCB(lv_timer_t* timer);
    static void SettingsSliderCB(lv_event_t* e);
    static const char* WeatherIcon(const char* desc);
};

#endif
