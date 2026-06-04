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
#include <esp_http_client.h>
#include <lvgl.h>
#include <cmath>
extern "C" void lv_draw_sw_rgb565_swap(void *buf, uint32_t buf_size);
#define TAG "CustomWatchS3"

// 面板互斥锁 — 必须用无捕获 lambda 作为 C 回调，所以用 static 变量中转
static SemaphoreHandle_t s_panel_mutex = nullptr;

// ==================== 16-bit 立体声双 I2S 编解码器 ====================
// I2S_NUM_0 TX → NS4168 功放，I2S_NUM_1 RX → MS4030 麦克风
class CustomWatchAudioCodec : public NoAudioCodec {
public:
    // Read: 从 16-bit stereo FIFO word 中提取左声道 (L/R=GND)
    int Read(int16_t* dest, int samples) override {
        std::vector<int32_t> buf(samples);
        size_t bytes_read = 0;
        if (i2s_channel_read(rx_handle_, buf.data(),
                              samples * sizeof(int32_t), &bytes_read,
                              pdMS_TO_TICKS(200)) != ESP_OK)
            return 0;
        int n = bytes_read / sizeof(int32_t);
        for (int i = 0; i < n; i++)
            dest[i] = (int16_t)(buf[i] & 0xFFFF);  // left=[15:0]
        return n;
    }

    // Write: 左右声道填入相同数据，消除基类 stereo 混叠杂音
    int Write(const int16_t* data, int samples) override {
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        int32_t vf = pow(double(output_volume_) / 100.0, 2) * 65536;
        std::vector<int32_t> buf(samples);
        for (int i = 0; i < samples; i++) {
            int64_t temp = int64_t(data[i]) * vf;
            int16_t audio = (temp > INT32_MAX) ? INT16_MAX :
                            (temp < INT32_MIN) ? -INT16_MAX :
                            (int16_t)(static_cast<int32_t>(temp) >> 16);
            buf[i] = ((int32_t)audio << 16) | ((int32_t)audio & 0xFFFF);
        }
        size_t written;
        i2s_channel_write(tx_handle_, buf.data(), samples * sizeof(int32_t),
                          &written, portMAX_DELAY);
        return written / sizeof(int32_t);
    }

    CustomWatchAudioCodec(int input_rate, int output_rate,
                          gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                          gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_din) {
        duplex_ = true;
        input_sample_rate_ = input_rate;
        output_sample_rate_ = output_rate;

        // TX: I2S_NUM_0 → NS4168 (16-bit 立体声)
        i2s_chan_config_t tx_chan = {
            .id = I2S_NUM_0, .role = I2S_ROLE_MASTER,
            .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true, .auto_clear_before_cb = false, .intr_priority = 0,
        };
        ESP_ERROR_CHECK(i2s_new_channel(&tx_chan, &tx_handle_, nullptr));

        i2s_std_config_t tx_cfg = {
            .clk_cfg = { .sample_rate_hz = (uint32_t)output_rate, .clk_src = I2S_CLK_SRC_DEFAULT,
                         .mclk_multiple = I2S_MCLK_MULTIPLE_256 },
            .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                          .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH,
                          .ws_width = I2S_DATA_BIT_WIDTH_16BIT, .ws_pol = false, .bit_shift = true },
            .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = spk_bclk, .ws = spk_ws, .dout = spk_dout,
                          .din = I2S_GPIO_UNUSED,
                          .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false } }
        };
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_cfg));

        // RX: I2S_NUM_1 → MS4030 (16-bit 立体声)
        i2s_chan_config_t rx_chan = {
            .id = I2S_NUM_1, .role = I2S_ROLE_MASTER,
            .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true, .auto_clear_before_cb = false, .intr_priority = 0,
        };
        ESP_ERROR_CHECK(i2s_new_channel(&rx_chan, nullptr, &rx_handle_));

        i2s_std_config_t rx_cfg = {
            .clk_cfg = { .sample_rate_hz = (uint32_t)input_rate, .clk_src = I2S_CLK_SRC_DEFAULT,
                         .mclk_multiple = I2S_MCLK_MULTIPLE_256 },
            .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                          .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH,
                          .ws_width = I2S_DATA_BIT_WIDTH_16BIT, .ws_pol = false, .bit_shift = true },
            .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = mic_bclk, .ws = mic_ws, .dout = I2S_GPIO_UNUSED,
                          .din = mic_din,
                          .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false } }
        };
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_cfg));

        ESP_LOGI(TAG, "Audio: TX=I2S0 RX=I2S1 (16bit stereo)");
    }
};

#define WEATHER_URL "http://wttr.in/Shenzhen?format=j1"
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
    {0x63, (uint8_t[]){0xFF}, 1, 0},     // HBM 亮度最大
    {0x58, (uint8_t[]){0x00}, 1, 10},    // 关闭电源节省模式（禁用APS自动睡眠）
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},  // 列 22-431 (410列+偏移22)
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},  // 行 0-501
    {0x36, (uint8_t[]){0x00}, 1, 0},     // MADCTL: 默认字节序，软件swap
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
    i2c_master_bus_handle_t touch_i2c_bus_;   // I2C_NUM_0 (触摸+传感器+RTC共用)
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    esp_lcd_panel_io_handle_t panel_io_;
    esp_lcd_panel_handle_t panel_;
    LcdDisplay* display_;
    WatchBacklight* backlight_;
    WatchFace* watch_face_;
    Bmm150* bmm150_;
    Qmi8658* qmi8658_;
    int step_count_;
    std::string weather_desc_;    // 天气描述（LVGL上下文消费）
    int weather_temp_;

    // ---- 电源初始化 ----
    void InitializePower() {
        // 充电状态检测 (CHECK_CHARGE) — 输入
        gpio_config_t charge_conf = {};
        charge_conf.pin_bit_mask = (1ULL << CHECK_CHARGE_PIN);
        charge_conf.mode = GPIO_MODE_INPUT;
        charge_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        charge_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        charge_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&charge_conf);

        // 一键开机控制 (OneCLINK_CTARTUP) — 输出
        gpio_config_t startup_conf = {};
        startup_conf.pin_bit_mask = (1ULL << ONE_CLICK_STARTUP_PIN);
        startup_conf.mode = GPIO_MODE_OUTPUT;
        startup_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        startup_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        startup_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&startup_conf);
        gpio_set_level(ONE_CLICK_STARTUP_PIN, 0);

        // 电池电压 ADC 检测 — 配置为模拟输入（ADC 初始化由 IDF 自动处理）
        gpio_config_t bat_adc_conf = {};
        bat_adc_conf.pin_bit_mask = (1ULL << BAT_CHECK_ADC_PIN);
        bat_adc_conf.mode = GPIO_MODE_DISABLE;
        bat_adc_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        bat_adc_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        bat_adc_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&bat_adc_conf);

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
        static int flush_count = 0;

        // PARTIAL模式每次flush ≤ 20行×全宽，静态缓冲区避免堆分配
        static uint8_t s_swap_buf[DISPLAY_WIDTH * 20 * sizeof(lv_color_t)];
        lv_display_set_flush_cb(disp, [](lv_display_t *d, const lv_area_t *a, uint8_t *px) {
            flush_count++;
            auto *p = (esp_lcd_panel_handle_t)lv_display_get_user_data(d);
            int w = a->x2 - a->x1 + 1;
            int h = a->y2 - a->y1 + 1;
            int sz = w * h * 2;
            // 获取面板锁，防止与防APS任务并发
            if (s_panel_mutex) xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
            if (sz <= (int)sizeof(s_swap_buf)) {
                memcpy(s_swap_buf, px, sz);
                lv_draw_sw_rgb565_swap(s_swap_buf, w * h);
                esp_lcd_panel_draw_bitmap(p, a->x1, a->y1, a->x2 + 1, a->y2 + 1, s_swap_buf);
            } else {
                esp_lcd_panel_draw_bitmap(p, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
            }
        });

        // 每5秒打印 LVGL flush 次数（诊断: 总数 + 近5秒增量）
        lv_timer_create([](lv_timer_t* t) {
            int prev = (int)(uintptr_t)lv_timer_get_user_data(t);
            int delta = flush_count - prev;
            lv_timer_set_user_data(t, (void*)(uintptr_t)flush_count);
            ESP_LOGI(TAG, "FLUSH诊断: 总计%d 近5秒+%d", flush_count, delta);
        }, 5000, nullptr);
        lv_display_add_event_cb(disp, lcd_rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);

        const esp_lcd_panel_io_callbacks_t cbs = {
            .on_color_trans_done = [](esp_lcd_panel_io_handle_t io,
                                      esp_lcd_panel_io_event_data_t *e, void *ctx) -> bool {
                lv_display_flush_ready((lv_display_t*)ctx);
                if (s_panel_mutex) xSemaphoreGive(s_panel_mutex);
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
        // 长按 BOOT 键 3 秒重启
        boot_button_.OnLongPress([this]() {
            ESP_LOGW(TAG, "BOOT 长按 → 重启");
            auto& app = Application::GetInstance();
            app.Schedule([&app]() { vTaskDelay(pdMS_TO_TICKS(500)); app.Reboot(); });
        });

        // 音量+ (单击+5, 长按+10)
        volume_up_button_.OnClick([this]() {
            auto* codec = GetAudioCodec();
            if (codec) {
                int vol = codec->output_volume() + 5;
                if (vol > 100) vol = 100;
                codec->SetOutputVolume(vol);
                if (watch_face_) watch_face_->ShowVolumeToast(vol);
                ESP_LOGI(TAG, "Volume: %d%%", vol);
            }
        });
        volume_up_button_.OnLongPress([this]() {
            auto* codec = GetAudioCodec();
            if (codec) {
                int vol = codec->output_volume() + 10;
                if (vol > 100) vol = 100;
                codec->SetOutputVolume(vol);
                if (watch_face_) watch_face_->ShowVolumeToast(vol);
                ESP_LOGI(TAG, "Volume: %d%%", vol);
            }
        });

        // 音量- (单击-5, 长按-10)
        volume_down_button_.OnClick([this]() {
            auto* codec = GetAudioCodec();
            if (codec) {
                int vol = codec->output_volume() - 5;
                if (vol < 0) vol = 0;
                codec->SetOutputVolume(vol);
                if (watch_face_) watch_face_->ShowVolumeToast(vol);
                ESP_LOGI(TAG, "Volume: %d%%", vol);
            }
        });
        volume_down_button_.OnLongPress([this]() {
            auto* codec = GetAudioCodec();
            if (codec) {
                int vol = codec->output_volume() - 10;
                if (vol < 0) vol = 0;
                codec->SetOutputVolume(vol);
                if (watch_face_) watch_face_->ShowVolumeToast(vol);
                ESP_LOGI(TAG, "Volume: %d%%", vol);
            }
        });
    }

    // ---- 传感器初始化（BMM150 + QMI8658 共用触摸 I2C，单个任务顺序初始化） ----
    void InitializeSensors() {
        xTaskCreate([](void* arg) {
            auto* self = static_cast<CustomWatchS3Board*>(arg);
            vTaskDelay(pdMS_TO_TICKS(1000));

            // === I2C 总线扫描（调试用：探测 0x08~0x77 所有地址） ===
            ESP_LOGI(TAG, "=== I2C Scan start ===");
            for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
                i2c_device_config_t probe_cfg = {
                    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                    .device_address = addr,
                    .scl_speed_hz = 100000,
                };
                i2c_master_dev_handle_t probe_dev = nullptr;
                esp_err_t err = i2c_master_bus_add_device(self->touch_i2c_bus_, &probe_cfg, &probe_dev);
                if (err == ESP_OK && probe_dev) {
                    uint8_t reg = 0x00;
                    uint8_t val = 0;
                    err = i2c_master_transmit_receive(probe_dev, &reg, 1, &val, 1, 100);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "I2C Scan: device at 0x%02X (reg0=0x%02X)", addr, val);
                    }
                    i2c_master_bus_rm_device(probe_dev);
                }
            }
            ESP_LOGI(TAG, "=== I2C Scan done ===");

            // BMM150 罗盘（先尝试地址 0x13，再尝试 0x10）
            {
                static const uint8_t bmm_candidates[] = {0x13, 0x10, 0x11, 0x12};
                for (uint8_t addr : bmm_candidates) {
                    auto* bmm = new Bmm150(self->touch_i2c_bus_, addr);
                    if (bmm->Init()) {
                        self->bmm150_ = bmm;
                        ESP_LOGI(TAG, "BMM150 OK (addr=0x%02X)", addr);
                        // 指南针定时器 (200ms 更新)
                        auto* ct = lv_timer_create([](lv_timer_t* t) {
                            auto* b = static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(t));
                            if (b->watch_face_ && b->bmm150_) {
                                float h = b->bmm150_->GetHeading();
                                if (h >= 0) b->watch_face_->UpdateCompass(h);
                            }
                        }, 200, self);
                        lv_timer_set_repeat_count(ct, -1);
                        break;  // 找到即停止
                    } else {
                        ESP_LOGW(TAG, "BMM150 not found at 0x%02X", addr);
                        delete bmm;
                    }
                }
                if (!self->bmm150_) {
                    ESP_LOGW(TAG, "BMM150 not found at any address");
                }
            }

            // QMI8658 六轴 IMU（I2C 地址 0x6B）
            {
                auto* qmi = new Qmi8658(self->touch_i2c_bus_, 0x6B);
                if (qmi->Init()) {
                    self->qmi8658_ = qmi;
                    auto* ctx = new ImuTaskCtx{
                        .imu = qmi,
                        .on_step = OnImuStep,
                        .on_wrist_up = OnImuWristUp,
                        .on_shake = OnImuShake,
                        .user_data = self,
                    };
                    xTaskCreate(imu_task, "imu", 4096, ctx, 2, nullptr);
                    ESP_LOGI(TAG, "QMI8658 OK, IMU started");
                } else {
                    ESP_LOGW(TAG, "QMI8658 not found - motion disabled");
                    delete qmi;
                }
            }
            vTaskDelete(nullptr);
        }, "sensors", 4096, this, 2, nullptr);
    }

    void StartWeatherTimer() {
        // 15秒后首次获取，之后每30分钟自动刷新
        auto* t = lv_timer_create([](lv_timer_t* timer) {
            StartWeatherFetch(static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(timer)));
            lv_timer_set_period(timer, 1800000);
        }, 15000, this);
        lv_timer_set_repeat_count(t, -1);
    }

public:
    // 天气数据存储 + lv_async_call 触发 LVGL 上下文安全更新
    static void WeatherUICB(void* user) {
        auto* self = static_cast<CustomWatchS3Board*>(user);
        if (self->watch_face_)
            self->watch_face_->UpdateWeather(self->weather_desc_.c_str(), self->weather_temp_);
    }
    void OnWeatherUpdate(const char* desc, int temp_c) {
        weather_desc_ = desc ? desc : "";
        weather_temp_ = temp_c;
        lv_async_call(WeatherUICB, this);
    }

    // IMU 回调（在 IMU 任务中调用，只存储步数，由 LVGL 定时器刷新显示）
    static void OnImuStep(void* user, int steps) {
        auto* self = static_cast<CustomWatchS3Board*>(user);
        self->step_count_ = steps;
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

    CustomWatchS3Board() : boot_button_(BOOT_BUTTON_GPIO),
                           volume_up_button_(VOLUME_UP_BUTTON_GPIO),
                           volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
                           watch_face_(nullptr),
                           bmm150_(nullptr), qmi8658_(nullptr), step_count_(0), weather_temp_(0) {
        s_panel_mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(s_panel_mutex);
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

            // 防 CO5300 APS 休眠：独立FreeRTOS任务，与LVGL通过信号量互斥
            {
                static esp_lcd_panel_handle_t s_keep_panel = self->panel_;
                xTaskCreate([](void* arg) {
                    auto* panel = (esp_lcd_panel_handle_t)arg;
                    uint16_t pixel = 0x0000;
                    bool flip = false;
                    while (true) {
                        flip = !flip;
                        pixel = flip ? 0x0000 : 0x0001;
                        if (s_panel_mutex) xSemaphoreTake(s_panel_mutex, pdMS_TO_TICKS(100));
                        esp_lcd_panel_draw_bitmap(panel, 0, 0, 1, 1, (uint8_t*)&pixel);
                        if (s_panel_mutex) xSemaphoreGive(s_panel_mutex);
                        vTaskDelay(pdMS_TO_TICKS(800));
                    }
                }, "keep", 4096, s_keep_panel, 10, nullptr);
            }
            // 初始化后立即全屏刷新，清除GRAM随机数据
            lv_obj_invalidate(lv_scr_act());

            // 天气：FreeRTOS 任务中做 HTTP，不阻塞调度器
            self->StartWeatherTimer();
            // 传感器（BMM150 + QMI8658 + 指南针定时器）
            self->InitializeSensors();

            // 步数刷新（LVGL 上下文更新，避免从 IMU 回调直接调用 LVGL 函数）
            {
                auto* t_step = lv_timer_create([](lv_timer_t* t_s) {
                    auto* b = static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(t_s));
                    if (b->watch_face_) b->watch_face_->UpdateSteps(b->step_count_);
                }, 2000, self);
                lv_timer_set_repeat_count(t_step, -1);  // 无限重复，每2秒刷新
            }

            // 触摸唤醒 (三个页面底部按钮)
            for (int pg = 0; pg < 3; pg++) {
                lv_obj_add_event_cb(self->watch_face_->GetTapArea(pg), [](lv_event_t* e) {
                    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
                        Application::GetInstance().ToggleChatState();
                }, LV_EVENT_CLICKED, nullptr);
            }

            // 状态轮询：idle 显示手表，聊天时隐藏 + 眨眼动画
            static lv_timer_t* blink_timer = nullptr;
            static bool was_showing_watch = true;
            lv_timer_create([](lv_timer_t* t2) {
                auto* b = static_cast<CustomWatchS3Board*>(lv_timer_get_user_data(t2));
                if (!b->watch_face_) return;
                auto s = Application::GetInstance().GetDeviceState();
                bool show_watch = (s == kDeviceStateIdle || s == kDeviceStateStarting ||
                                   s == kDeviceStateWifiConfiguring || s == kDeviceStateActivating);

                // 模式切换时全屏刷新，清除GRAM残留（手表↔聊天）
                if (was_showing_watch != show_watch) {
                    was_showing_watch = show_watch;
                    lv_obj_invalidate(lv_scr_act());
                }

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
        static CustomWatchAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            SPEAKER_I2S_GPIO_BCLK, SPEAKER_I2S_GPIO_LRC, SPEAKER_I2S_GPIO_DOUT,
            MIC_I2S_GPIO_BCLK, MIC_I2S_GPIO_WS, MIC_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }
};

// ==================== 天气获取（esp_http_client + 10秒超时 + 跳过证书验证） ====================
static esp_err_t _weather_event_handler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        auto* body = static_cast<std::string*>(evt->user_data);
        body->append((char*)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static void StartWeatherFetch(CustomWatchS3Board* board) {
    xTaskCreate([](void* arg) {
        auto* board = static_cast<CustomWatchS3Board*>(arg);
        ESP_LOGI(TAG, "天气任务启动");

        while (true) {
            std::string body;
            esp_http_client_config_t cfg = {};
            cfg.url = WEATHER_URL;
            cfg.event_handler = _weather_event_handler;
            cfg.user_data = &body;
            cfg.timeout_ms = 20000;  // 20s 超时（原60s太长）
            cfg.skip_cert_common_name_check = true;

            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            esp_err_t err = esp_http_client_perform(client);
            int status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            ESP_LOGI(TAG, "天气: HTTP状态=%d 错误=%d 数据=%d字节", status, err, body.size());

            if (err == ESP_OK && status == 200 && !body.empty()) {
                cJSON* root = cJSON_Parse(body.c_str());
                if (root) {
                    cJSON* current = cJSON_GetObjectItem(root, "current_condition");
                    if (current && cJSON_GetArraySize(current) > 0) {
                        cJSON* cond = cJSON_GetArrayItem(current, 0);
                        cJSON* temp = cJSON_GetObjectItem(cond, "temp_C");
                        cJSON* desc_arr = cJSON_GetObjectItem(cond, "weatherDesc");
                        const char* desc = "";
                        if (desc_arr && cJSON_GetArraySize(desc_arr) > 0)
                            desc = cJSON_GetObjectItem(cJSON_GetArrayItem(desc_arr, 0), "value")->valuestring;
                        if (temp && desc) {
                            int temp_c = atoi(temp->valuestring);
                            ESP_LOGI(TAG, "天气: %s %d°C", desc, temp_c);
                            board->OnWeatherUpdate(desc, temp_c);
                        }
                    }
                    cJSON_Delete(root);
                }
                break; // 成功退出
            }
            ESP_LOGW(TAG, "天气获取失败，立即重试");
        }
        vTaskDelete(nullptr);
    }, "weather", 8192, board, 1, nullptr);
}

DECLARE_BOARD(CustomWatchS3Board);
