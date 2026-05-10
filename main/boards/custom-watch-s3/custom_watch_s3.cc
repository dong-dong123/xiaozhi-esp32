#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"
#include "watch_face.h"
#include "bmm150.h"
#include "qmi8658.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "device_state.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch_cst820.h>
#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <http.h>
#include <lvgl.h>
extern "C" void lv_draw_sw_rgb565_swap(void *buf, uint32_t buf_size);
#define TAG "CustomWatchS3"

// NS4168 专用 16-bit I2S：TX 立体声双声道输出，RX 只读左声道（MS4030 右声道三态=噪声）
class NS4168AudioCodec : public NoAudioCodec {
public:
    NS4168AudioCodec(int input_rate, int output_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
        duplex_ = true;
        input_sample_rate_ = input_rate;
        output_sample_rate_ = output_rate;

        i2s_chan_config_t chan_cfg = {
            .id = I2S_NUM_0, .role = I2S_ROLE_MASTER,
            .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true, .auto_clear_before_cb = false, .intr_priority = 0,
        };
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

        // TX: 16-bit 立体声双声道 → NS4168
        i2s_std_config_t tx_cfg = {
            .clk_cfg = { .sample_rate_hz = (uint32_t)output_rate, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256 },
            .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                          .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH,
                          .ws_width = I2S_DATA_BIT_WIDTH_16BIT, .ws_pol = false, .bit_shift = true },
            .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = bclk, .ws = ws, .dout = dout, .din = I2S_GPIO_UNUSED,
                          .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false } }
        };
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_cfg));

        // RX: 16-bit 双声道 ← MS4030（完全对齐 ATK 板配置）
        i2s_std_config_t rx_cfg = {
            .clk_cfg = { .sample_rate_hz = (uint32_t)output_rate, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256 },
            .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                          .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH,
                          .ws_width = I2S_DATA_BIT_WIDTH_16BIT, .ws_pol = false, .bit_shift = true },
            .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = bclk, .ws = ws, .dout = I2S_GPIO_UNUSED, .din = din,
                          .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false } }
        };
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_cfg));

        ESP_LOGI(TAG, "NS4168 Duplex: TX+RX 16bit BOTH");
    }
};

#define WEATHER_URL "https://wttr.in/Shenzhen?format=j1"
#define WEATHER_REFRESH_MS (30 * 60 * 1000)

class LcdDisplayWrapper : public LcdDisplay {
public:
    LcdDisplayWrapper(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t p, int w, int h)
        : LcdDisplay(io, p, w, h) {}
};

class CustomWatchS3Board;
static void StartWeatherFetch(CustomWatchS3Board* board);

// ==================== CO5300 原生初始化命令 (410x502) ====================
#define LCD_OPCODE_WRITE_CMD   (0x02ULL)
#define LCD_OPCODE_READ_CMD    (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600},  // Sleep out, 600ms
    {0xFE, (uint8_t[]){0x20}, 1, 0},     // Bank 0x20
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},     // Bank 0x00
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},     // RGB565
    {0x35, (uint8_t[]){0x00}, 1, 0},     // TE off
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},     // 亮度最大
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},  // 列 22-431 (410列+偏移22)
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},  // 行 0-501
    {0x36, (uint8_t[]){0x00}, 1, 0},     // MADCTL: 无BGR，靠lv_draw_sw_rgb565_swap
    {0x29, (uint8_t[]){0x00}, 0, 600},   // Display on, 600ms
};

// ==================== LVGL rounder（偶数对齐） ====================
static void lcd_rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

// ==================== 背光 ====================
class WatchBacklight : public Backlight {
private:
    esp_lcd_panel_io_handle_t panel_io_;
public:
    WatchBacklight(esp_lcd_panel_io_handle_t io) : Backlight(), panel_io_(io) {}
protected:
    virtual void SetBrightnessImpl(uint8_t brightness) override {
        // 亮度在 init 中已设置为 0xFF 最大，不在此处重复发送
    }
};

// ==================== 主板卡类 ====================
class CustomWatchS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t touch_i2c_bus_;   // I2C_NUM_0
    i2c_master_bus_handle_t sensor_i2c_bus_;  // I2C_NUM_1
    Button boot_button_;
    esp_lcd_panel_io_handle_t panel_io_;
    esp_lcd_panel_handle_t panel_;
    LcdDisplay* display_;
    WatchBacklight* backlight_;
    WatchFace* watch_face_;
    Bmm150* bmm150_;
    Qmi8658* qmi8658_;
    int step_count_;

    // ---- 电源初始化 ----
    void InitializePower() {
        // 屏幕电源使能
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << DISPLAY_VCI_EN);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(DISPLAY_VCI_EN, 1);

        // GPIO0 加内部上拉（防按键抖动误触下载模式）
        gpio_config_t boot_conf = {};
        boot_conf.pin_bit_mask = (1ULL << GPIO_NUM_0);
        boot_conf.mode = GPIO_MODE_INPUT;
        boot_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        boot_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        boot_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&boot_conf);

        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // ---- QSPI 总线 ----
    void InitializeQspi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = DISPLAY_QSPI_CLK;
        buscfg.data0_io_num = DISPLAY_QSPI_SIO0;
        buscfg.data1_io_num = DISPLAY_QSPI_SIO1;
        buscfg.data2_io_num = DISPLAY_QSPI_SIO2;
        buscfg.data3_io_num = DISPLAY_QSPI_SIO3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // ---- QSPI AMOLED 显示屏 ----
    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Init CO5300 QSPI panel IO");
        esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(
            DISPLAY_QSPI_CS, nullptr, nullptr);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));
        panel_io_ = panel_io;

        ESP_LOGI(TAG, "Init CO5300 driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = vendor_specific_init,
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = { .use_qspi_interface = 1 },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = DISPLAY_QSPI_RST,
            .rgb_ele_order = DISPLAY_RGB_ORDER,
            .bits_per_pixel = 16,
            .vendor_config = (void*)&vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        panel_ = panel;  // 保存句柄，供防休眠定时器使用
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 22, 0));

        // 暗色主题
        {
            Settings settings("display", true);
            settings.SetString("theme", "dark");
        }

        // ———— 参考代码方式 LVGL 初始化 ————
        lv_init();
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        port_cfg.task_priority = 1;
        port_cfg.timer_period_ms = 50;
        lvgl_port_init(&port_cfg);

        size_t buf_sz = DISPLAY_WIDTH * 20 * sizeof(lv_color_t);
        void *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
        void *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
        assert(buf1 && buf2);

        lv_display_t *disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_user_data(disp, panel);
        lv_display_set_flush_cb(disp, [](lv_display_t *d, const lv_area_t *a, uint8_t *px) {
            auto *p = (esp_lcd_panel_handle_t)lv_display_get_user_data(d);
            int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
            size_t sz = w * h * sizeof(uint16_t);
            // 使用栈上小缓冲或堆分配避免原地修改 LVGL 渲染缓冲
            static uint8_t* swap_buf = nullptr;
            static size_t swap_buf_sz = 0;
            if (sz > swap_buf_sz) {
                if (swap_buf) heap_caps_free(swap_buf);
                swap_buf_sz = sz;
                swap_buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_DMA);
            }
            if (swap_buf) {
                memcpy(swap_buf, px, sz);
                lv_draw_sw_rgb565_swap(swap_buf, w * h);
                esp_lcd_panel_draw_bitmap(p, a->x1, a->y1, a->x2 + 1, a->y2 + 1, swap_buf);
            } else {
                esp_lcd_panel_draw_bitmap(p, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
            }
        });
        lv_display_add_event_cb(disp, lcd_rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);

        const esp_lcd_panel_io_callbacks_t cbs = {
            .on_color_trans_done = [](esp_lcd_panel_io_handle_t io,
                                       esp_lcd_panel_io_event_data_t *e, void *ctx) -> bool {
                lv_display_flush_ready((lv_display_t*)ctx);
                return false;
            }
        };
        esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, disp);

        // 暗色主题
        { Settings s("display", true); s.SetString("theme", "dark"); }

        display_ = new LcdDisplayWrapper(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT);

        // 确保亮度最大（QSPI 命令格式须与 CO5300 驱动 tx_param 一致: 0x0200_51_00 + data）
        {
            uint8_t b = 0xFF;
            uint32_t cmd = (LCD_OPCODE_WRITE_CMD << 24) | (0x51UL << 8);
            ESP_LOGI(TAG, "BRIGHTNESS: sending QSPI cmd=0x%08lX data=0x%02X", cmd, b);
            esp_lcd_panel_io_tx_param(panel_io, cmd, &b, 1);
        }

        backlight_ = new WatchBacklight(panel_io);
        lv_timer_create([](lv_timer_t* t) {
            auto* bl = static_cast<WatchBacklight*>(lv_timer_get_user_data(t));
            bl->RestoreBrightness();
            lv_timer_del(t);
        }, 300, backlight_);
    }

    // ---- I2C_NUM_0: 触摸 ----
    void InitializeTouchI2c() {
        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port = I2C_NUM_0;
        cfg.sda_io_num = TOUCH_I2C_SDA;
        cfg.scl_io_num = TOUCH_I2C_SCL;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &touch_i2c_bus_));
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp = nullptr;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1, .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = TOUCH_RST, .int_gpio_num = TOUCH_INT,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        };
        esp_lcd_panel_io_handle_t tp_io = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
        tp_io_cfg.scl_speed_hz = 400 * 1000;

        esp_err_t ret = esp_lcd_new_panel_io_i2c(touch_i2c_bus_, &tp_io_cfg, &tp_io);
        if (ret != ESP_OK) { ESP_LOGW(TAG, "Touch I2C io failed, skip"); return; }
        ret = esp_lcd_touch_new_i2c_cst820(tp_io, &tp_cfg, &tp);
        if (ret != ESP_OK) { ESP_LOGW(TAG, "Touch init failed, skip"); return; }
        const lvgl_port_touch_cfg_t lv_touch_cfg = { .disp = lv_display_get_default(), .handle = tp };
        lvgl_port_add_touch(&lv_touch_cfg);
        ESP_LOGI(TAG, "Touch OK");
    }

    // ---- I2C_NUM_1: 传感器 (BMM150 + QMI8658 共用) ----
    void InitializeSensorI2c() {
        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port = I2C_NUM_1;
        cfg.sda_io_num = SENSOR_I2C_SDA;
        cfg.scl_io_num = SENSOR_I2C_SCL;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &sensor_i2c_bus_));
        ESP_LOGI(TAG, "Sensor I2C bus ready (BMM150 + QMI8658)");
    }

    // ---- 按键 ----
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    // ---- 传感器初始化（P5+P6，失败不阻塞启动） ----
    void InitializeSensors() {
        // BMM150 磁力计（在 FreeRTOS 任务中尝试，避免阻塞主线程）
        xTaskCreate([](void* arg) {
            auto* self = static_cast<CustomWatchS3Board*>(arg);
            vTaskDelay(pdMS_TO_TICKS(500));  // 等网络/WiFi 先启动

            auto* bmm = new Bmm150(self->sensor_i2c_bus_, 0x10);
            if (bmm->Init()) {
                self->bmm150_ = bmm;
                ESP_LOGI(TAG, "BMM150 OK");
            } else {
                ESP_LOGW(TAG, "BMM150 not found - compass disabled");
                delete bmm;
            }
            vTaskDelete(nullptr);
        }, "bmm_init", 2048, this, 1, nullptr);

        // QMI8658 六轴 IMU（同样在任务中初始化）
        xTaskCreate([](void* arg) {
            auto* self = static_cast<CustomWatchS3Board*>(arg);
            vTaskDelay(pdMS_TO_TICKS(800));

            auto* qmi = new Qmi8658(self->sensor_i2c_bus_, 0x6A);
            if (qmi->Init()) {
                self->qmi8658_ = qmi;
                auto* ctx = new ImuTaskCtx{
                    .imu = qmi,
                    .on_step = OnImuStep,
                    .on_wrist_up = OnImuWristUp,
                    .on_shake = OnImuShake,
                    .user_data = self,
                };
                xTaskCreate(imu_task, "imu", 4096, ctx, 1, nullptr);
                ESP_LOGI(TAG, "QMI8658 OK, IMU started");
            } else {
                ESP_LOGW(TAG, "QMI8658 not found - motion disabled");
                delete qmi;
            }
            vTaskDelete(nullptr);
        }, "qmi_init", 2048, this, 1, nullptr);

        // 指南针刷新定时器（延迟 2 秒启动，等 BMM150 就绪）
        lv_timer_create([](lv_timer_t* t) {
            auto* self = static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(t));
            if (self->bmm150_ && self->watch_face_) {
                float heading = self->bmm150_->GetHeading();
                if (heading >= 0) self->watch_face_->UpdateCompass(heading);
            }
        }, 100, this);
    }

    // ---- 手表界面 + 触摸唤醒（延迟到 LVGL 就绪后创建） ----
    void InitializeWatchFace() {
        lv_timer_create([](lv_timer_t* t) {
            auto* self = static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(t));
            lv_timer_del(t);

            lv_obj_t* parent = lv_scr_act();
            self->watch_face_ = new WatchFace(parent);

            // 触摸唤醒
            lv_obj_add_event_cb(self->watch_face_->GetTapArea(), [](lv_event_t* e) {
                if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                    Application::GetInstance().ToggleChatState();
                }
            }, LV_EVENT_CLICKED, nullptr);

            // 状态轮询
            lv_timer_create([](lv_timer_t* t2) {
                auto* b = static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(t2));
                auto state = Application::GetInstance().GetDeviceState();
                if (state == kDeviceStateIdle || state == kDeviceStateStarting ||
                    state == kDeviceStateWifiConfiguring || state == kDeviceStateActivating) {
                    b->watch_face_->Show();
                } else {
                    b->watch_face_->Hide();
                }
            }, 500, self);
        }, 200, this);  // 延迟 200ms，等 LVGL 渲染首帧完成
    }

    void StartWeatherTimer() {
        // 延迟 5 秒后首次获取（等网络就绪），之后每 30 分钟刷新
        lv_timer_t* t = lv_timer_create([](lv_timer_t* timer) {
            StartWeatherFetch(static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(timer)));
            // 首次触发后改为 30 分钟周期
            lv_timer_set_period(timer, WEATHER_REFRESH_MS);
        }, 5000, this);
        lv_timer_set_repeat_count(t, 1);
    }

public:
    void OnWeatherUpdate(const char* desc, int temp_c) {
        if (watch_face_) watch_face_->UpdateWeather(desc, temp_c);
    }

    // IMU 回调（在 IMU 任务中调用，通过 Schedule 投递到主线程）
    static void OnImuStep(void* user, int steps) {
        auto* self = static_cast<CustomWatchS3Board*>(user);
        Application::GetInstance().Schedule([self, steps]() {
            if (self->watch_face_) self->watch_face_->UpdateSteps(steps);
        });
    }
    static void OnImuWristUp(void* user) {
        auto* self = static_cast<CustomWatchS3Board*>(user);
        Application::GetInstance().Schedule([self]() {
            if (self->backlight_) self->backlight_->RestoreBrightness();
        });
    }
    static void OnImuShake(void* user) {
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().ToggleChatState();
        });
    }

    CustomWatchS3Board() : boot_button_(BOOT_BUTTON_GPIO), watch_face_(nullptr),
                           bmm150_(nullptr), qmi8658_(nullptr), step_count_(0) {
        ESP_LOGI(TAG, "=== Board init start ===");
        InitializePower();
        ESP_LOGI(TAG, "Power OK");
        InitializeQspi();
        ESP_LOGI(TAG, "QSPI OK");
        InitializeDisplay();
        ESP_LOGI(TAG, "Display OK");
        InitializeTouchI2c();
        ESP_LOGI(TAG, "Touch I2C OK");
        InitializeTouch();
        ESP_LOGI(TAG, "Touch OK");
        InitializeSensorI2c();
        ESP_LOGI(TAG, "Sensor I2C OK");
        InitializeButtons();
        ESP_LOGI(TAG, "Buttons OK");
        // 延迟创建手表界面（等 LVGL 渲染稳定），传感器在 WatchFace 创建后初始化
        lv_timer_create([](lv_timer_t* t) {
            auto* self = static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(t));
            lv_timer_del(t);

            lv_obj_t* parent = lv_scr_act();
            if (!parent) { ESP_LOGE(TAG, "No active screen!"); return; }
            self->watch_face_ = new WatchFace(parent);
            ESP_LOGI(TAG, "WatchFace created");

            // 防 CO5300 自动休眠：每5秒用 esp_timer（独立于 LVGL）发送 Display On
            esp_timer_create_args_t awake_args = {};
            awake_args.callback = [](void* arg) {
                esp_lcd_panel_disp_on_off((esp_lcd_panel_handle_t)arg, true);
            };
            awake_args.arg = self->panel_;
            awake_args.dispatch_method = ESP_TIMER_TASK;
            awake_args.name = "co5300_awake";
            esp_timer_handle_t awake_timer = nullptr;
            esp_timer_create(&awake_args, &awake_timer);
            esp_timer_start_periodic(awake_timer, 5000000);  // 5秒

            // TODO: 传感器和天气在后续 PR 中逐步启用
            // self->InitializeSensors();
            // self->StartWeatherTimer();

            // 触摸唤醒
            lv_obj_add_event_cb(self->watch_face_->GetTapArea(), [](lv_event_t* e) {
                if (lv_event_get_code(e) == LV_EVENT_CLICKED)
                    Application::GetInstance().ToggleChatState();
            }, LV_EVENT_CLICKED, nullptr);

            // 状态轮询：idle 显示手表，聊天时隐藏 + 眨眼动画
            static lv_timer_t* blink_timer = nullptr;
            lv_timer_create([](lv_timer_t* t2) {
                auto* b = static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(t2));
                if (!b->watch_face_) return;
                auto s = Application::GetInstance().GetDeviceState();
                bool show_watch = (s == kDeviceStateIdle || s == kDeviceStateStarting ||
                                   s == kDeviceStateWifiConfiguring || s == kDeviceStateActivating);

                if (!show_watch) {
                    b->watch_face_->Hide();
                    // 聊天时启动眨眼动画
                    if (!blink_timer) {
                        blink_timer = lv_timer_create([](lv_timer_t* bt) {
                            static bool eye_open = true;
                            eye_open = !eye_open;
                            auto* display = Board::GetInstance().GetDisplay();
                            if (display) display->SetEmotion(eye_open ? "neutral" : "winking");
                        }, 2000, nullptr);
                        lv_timer_set_repeat_count(blink_timer, -1);
                    }
                } else {
                    b->watch_face_->Show();
                    if (blink_timer) {
                        lv_timer_del(blink_timer);
                        blink_timer = nullptr;
                    }
                }
            }, 500, self);
        }, 500, this);  // 延迟 500ms

        ESP_LOGI(TAG, "=== Board init done, free heap: %lu ===", esp_get_free_heap_size());
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }
};

// ==================== 天气获取（P4: wttr.in 免费API） ====================
static void StartWeatherFetch(CustomWatchS3Board* board) {
    Application::GetInstance().Schedule([board]() {
        auto network = Board::GetInstance().GetNetwork();
        if (!network) return;

        auto http = network->CreateHttp(0);
        http->SetHeader("User-Agent", "XiaozhiWatch/1.0");
        if (!http->Open("GET", WEATHER_URL)) return;

        std::string body = http->ReadAll();
        http->Close();
        if (body.empty()) return;

        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) return;

        cJSON* current = cJSON_GetObjectItem(root, "current_condition");
        if (current && cJSON_GetArraySize(current) > 0) {
            cJSON* cond = cJSON_GetArrayItem(current, 0);
            cJSON* temp = cJSON_GetObjectItem(cond, "temp_C");
            cJSON* desc_arr = cJSON_GetObjectItem(cond, "weatherDesc");
            const char* desc = "";
            if (desc_arr && cJSON_GetArraySize(desc_arr) > 0) {
                desc = cJSON_GetObjectItem(cJSON_GetArrayItem(desc_arr, 0), "value")->valuestring;
            }
            if (temp && desc) {
                ESP_LOGI(TAG, "Weather: %s %dC", desc, (int)temp->valuedouble);
                board->OnWeatherUpdate(desc, (int)temp->valuedouble);
            }
        }
        cJSON_Delete(root);
    });
}

DECLARE_BOARD(CustomWatchS3Board);
