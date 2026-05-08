#include "qmi8658.h"
#include "application.h"
#include <esp_log.h>
#include <cmath>
#include <cstring>

#define TAG "QMI8658"

// QMI8658 寄存器
#define REG_WHO_AM_I   0x00
#define REG_CTRL1      0x02
#define REG_CTRL2      0x03
#define REG_CTRL3      0x04
#define REG_CTRL5      0x06
#define REG_CTRL7      0x08
#define REG_STATUS     0x2E
#define REG_ACC_L      0x35
#define REG_TEMP_L     0x33

#define WHO_AM_I_VAL   0x05

Qmi8658::Qmi8658(i2c_master_bus_handle_t bus, uint8_t addr)
    : bus_(bus), dev_(nullptr), addr_(addr), acc_scale_(0), gyr_scale_(0)
{}

Qmi8658::~Qmi8658() {
    if (dev_) i2c_master_bus_rm_device(dev_);
}

uint8_t Qmi8658::ReadReg(uint8_t reg) {
    uint8_t val = 0;
    i2c_master_transmit_receive(dev_, &reg, 1, &val, 1, 100);
    return val;
}

void Qmi8658::WriteReg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_master_transmit(dev_, buf, 2, 100);
}

void Qmi8658::ReadMulti(uint8_t reg, uint8_t* data, uint8_t len) {
    i2c_master_transmit_receive(dev_, &reg, 1, data, len, 100);
}

bool Qmi8658::Init() {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr_,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus_, &dev_cfg, &dev_) != ESP_OK) {
        ESP_LOGE(TAG, "I2C add failed");
        return false;
    }

    // 软复位
    WriteReg(REG_CTRL2, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(30));

    uint8_t id = ReadReg(REG_WHO_AM_I);
    if (id != WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "Bad WHO_AM_I: 0x%02X", id);
        return false;
    }
    ESP_LOGI(TAG, "WHO_AM_I OK");

    // 配置加速度计: ±8g, ODR 100Hz
    WriteReg(REG_CTRL1, 0x60);
    // 配置陀螺仪: ±2048dps, ODR 100Hz
    WriteReg(REG_CTRL7, 0x60);
    // 数据格式: 16-bit
    WriteReg(REG_CTRL5, 0x00);
    // 使能加速度计 + 陀螺仪
    WriteReg(REG_CTRL3, 0x03);

    // 比例因子
    acc_scale_ = 8.0f * 9.80665f / 32768.0f;     // m/s² per LSB
    gyr_scale_ = 2048.0f * (float)M_PI / 180.0f / 32768.0f;  // rad/s per LSB

    ESP_LOGI(TAG, "Init OK (acc=±8g, gyr=±2048dps, 100Hz)");
    return true;
}

bool Qmi8658::Read(ImuData& data) {
    // 检查数据就绪
    uint8_t status = ReadReg(REG_STATUS);
    if (!(status & 0x01)) return false;

    uint8_t raw[12];
    ReadMulti(REG_ACC_L, raw, 12);

    int16_t ax = (int16_t)(raw[0] | (raw[1] << 8));
    int16_t ay = (int16_t)(raw[2] | (raw[3] << 8));
    int16_t az = (int16_t)(raw[4] | (raw[5] << 8));
    int16_t gx = (int16_t)(raw[6] | (raw[7] << 8));
    int16_t gy = (int16_t)(raw[8] | (raw[9] << 8));
    int16_t gz = (int16_t)(raw[10]| (raw[11]<< 8));

    data.acc_x = ax * acc_scale_;
    data.acc_y = ay * acc_scale_;
    data.acc_z = az * acc_scale_;
    data.gyr_x = gx * gyr_scale_;
    data.gyr_y = gy * gyr_scale_;
    data.gyr_z = gz * gyr_scale_;
    data.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    return true;
}

// ==================== IMU 功能任务 ====================

void imu_task(void* arg) {
    auto* ctx = static_cast<ImuTaskCtx*>(arg);
    Qmi8658* imu = ctx->imu;

    int step_count = 0;
    bool step_high = false;
    uint32_t last_step_time = 0;

    float shake_buf[10] = {};
    int shake_idx = 0;
    bool wrist_up = false;

    ESP_LOGI(TAG, "IMU task started");

    while (true) {
        ImuData data;
        if (!imu->Read(data)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        float acc_mag = sqrtf(data.acc_x * data.acc_x +
                              data.acc_y * data.acc_y +
                              data.acc_z * data.acc_z);

        // ---- 抬腕亮屏 ----
        if (data.acc_z > 7.0f && !wrist_up) {
            wrist_up = true;
            if (ctx->on_wrist_up)
                ctx->on_wrist_up(ctx->user_data);
        }
        if (data.acc_z < 2.0f) wrist_up = false;

        // ---- 计步 ----
        if (acc_mag > 12.0f && !step_high &&
            (data.timestamp_ms - last_step_time > 200)) {
            step_high = true;
            step_count++;
            last_step_time = data.timestamp_ms;
            if (ctx->on_step)
                ctx->on_step(ctx->user_data, step_count);
        }
        if (acc_mag < 10.5f) step_high = false;

        // ---- 摇一摇唤醒 ----
        shake_buf[shake_idx] = acc_mag;
        shake_idx = (shake_idx + 1) % 10;
        float mean = 0;
        for (int i = 0; i < 10; i++) mean += shake_buf[i];
        mean /= 10.0f;
        float var = 0;
        for (int i = 0; i < 10; i++) {
            float d = shake_buf[i] - mean;
            var += d * d;
        }
        var /= 10.0f;
        if (var > 30.0f) {
            if (ctx->on_shake)
                ctx->on_shake(ctx->user_data);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
