#include "qmi8658.h"
#include "application.h"
#include <esp_log.h>
#include <cmath>
#include <cstring>

#define TAG "QMI8658"

// QMI8658 寄存器（对照官方数据手册和参考驱动）
#define REG_WHO_AM_I   0x00   // Chip ID
#define REG_REV_ID     0x01   // Revision
#define REG_CTRL1      0x02   // 串行接口 + 地址自增
#define REG_CTRL2      0x03   // 加速度计配置（量程+ODR）
#define REG_CTRL3      0x04   // 陀螺仪配置（量程+ODR）
#define REG_CTRL5      0x06   // LPF 低通滤波
#define REG_CTRL7      0x08   // 传感器使能
#define REG_CTRL8      0x09   // 保留
#define REG_STATUS     0x2E   // 数据就绪状态
#define REG_ACC_L      0x35   // 加速度 X 低字节（连续12字节）
#define REG_TEMP_L     0x33   // 温度
#define REG_RESET      0x60   // 软复位

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
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus_, &dev_cfg, &dev_) != ESP_OK) {
        ESP_LOGE(TAG, "I2C add failed");
        return false;
    }

    // 检查芯片ID（QMI8658A=0x05, QMI8658C=0x30）
    uint8_t id = ReadReg(REG_WHO_AM_I);
    if (id != 0x05 && id != 0x30) {
        ESP_LOGE(TAG, "Bad WHO_AM_I: 0x%02X (expected 0x05 or 0x30)", id);
        return false;
    }
    ESP_LOGI(TAG, "WHO_AM_I OK (0x%02X)", id);

    // 参照 Arduino QMI8658C 驱动：先关→配→开 顺序
    // 1. CTRL1: I2C 模式，地址自增
    WriteReg(REG_CTRL1, 0x40);

    // 2. CTRL7: 先关闭所有传感器
    WriteReg(REG_CTRL7, 0x00);

    // 3. CTRL2: 加速度计 ±2g, ODR 500Hz
    WriteReg(REG_CTRL2, 0x04);

    // 4. CTRL3: 陀螺仪 ±2048dps, ODR 500Hz
    WriteReg(REG_CTRL3, 0x64);

    // 5. CTRL5: 使能低通滤波
    WriteReg(REG_CTRL5, 0x11);

    // 6. CTRL7: 使能加速度计 + 陀螺仪
    WriteReg(REG_CTRL7, 0x03);

    // 传感器稳定延迟（Arduino参考用2000ms）
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 验证关键寄存器写入
    uint8_t v_ctrl1 = ReadReg(REG_CTRL1);
    uint8_t v_ctrl7 = ReadReg(REG_CTRL7);
    ESP_LOGI(TAG, "Verify: CTRL1=0x%02X CTRL7=0x%02X", v_ctrl1, v_ctrl7);

    // 比例因子（±2g → 2*9.80665/32768）
    acc_scale_ = 2.0f * 9.80665f / 32768.0f;
    gyr_scale_ = 2048.0f * (float)M_PI / 180.0f / 32768.0f;

    ESP_LOGI(TAG, "Init OK (acc=±2g, gyr=±2048dps, 500Hz)");
    return true;
}

bool Qmi8658::Read(ImuData& data) {
    // 检查加速度数据就绪（QMI8658C 可能 gyro 就绪较慢，先只检查 accel）
    uint8_t status = ReadReg(REG_STATUS);
    static int dbg_cnt = 0;
    if (++dbg_cnt <= 5) ESP_LOGI(TAG, "Status: 0x%02X", status);
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

    bool wrist_up = false;

    // ---- 摇一摇：检测完整来回周期 ----
    uint32_t shake_cycle_times[8] = {0};
    int shake_cycle_idx = 0;
    bool shake_high = false;
    bool shake_had_drop = false;
    uint32_t shake_cooldown = 0;

    ESP_LOGI(TAG, "IMU task started");

    uint32_t dbg_time = 0;
    while (true) {
        ImuData data;
        if (!imu->Read(data)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        float acc_mag = sqrtf(data.acc_x * data.acc_x +
                              data.acc_y * data.acc_y +
                              data.acc_z * data.acc_z);

        // 过滤传感器饱和/错误值（±2g量程下饱和值约为-19.6）
        if (data.acc_x < -16.0f || data.acc_y < -16.0f || data.acc_z < -16.0f ||
            data.acc_x > 16.0f || data.acc_y > 16.0f || data.acc_z > 16.0f) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 每10秒打印加速度值用于调试
        if (data.timestamp_ms - dbg_time > 10000) {
            dbg_time = data.timestamp_ms;
            ESP_LOGI(TAG, "ACC: x=%.1f y=%.1f z=%.1f mag=%.2f",
                     data.acc_x, data.acc_y, data.acc_z, acc_mag);
        }

        // ---- 抬腕亮屏 ----
        if (data.acc_z > 7.0f && !wrist_up) {
            wrist_up = true;
            if (ctx->on_wrist_up)
                ctx->on_wrist_up(ctx->user_data);
        }
        if (data.acc_z < 2.0f) wrist_up = false;

        // ---- 计步（阈值 13.0，需明显步伐） ----
        if (acc_mag > 13.0f && !step_high &&
            (data.timestamp_ms - last_step_time > 500)) {
            step_high = true;
            step_count++;
            last_step_time = data.timestamp_ms;
            if (ctx->on_step)
                ctx->on_step(ctx->user_data, step_count);
        }
        if (acc_mag < 11.0f) step_high = false;

        // ---- 摇一摇唤醒（13g→10g→13g 为一个周期，1s内≥5次，5s冷却） ----
        {
            if (acc_mag > 13.0f && !shake_high) {
                shake_high = true;
                if (shake_had_drop) {
                    shake_had_drop = false;
                    shake_cycle_times[shake_cycle_idx % 8] = data.timestamp_ms;
                    shake_cycle_idx++;
                    int count = 0;
                    for (int i = 0; i < 8; i++) {
                        if (shake_cycle_times[i] != 0 &&
                            (int32_t)(data.timestamp_ms - shake_cycle_times[i]) < 1000) {
                            count++;
                        }
                    }
                    if (count >= 5 && ctx->on_shake &&
                        (int32_t)(data.timestamp_ms - shake_cooldown) > 5000) {
                        ctx->on_shake(ctx->user_data);
                        shake_cooldown = data.timestamp_ms;
                        for (int i = 0; i < 8; i++) shake_cycle_times[i] = 0;
                        shake_cycle_idx = 0;
                    }
                }
            }
            if (acc_mag < 10.0f && shake_high) {
                shake_high = false;
                shake_had_drop = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
