#include "bmm150.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstring>

#define TAG "BMM150"

// BMM150 寄存器
#define REG_CHIP_ID     0x40
#define REG_DATA_X_LSB  0x42
#define REG_POWER_CTRL  0x4B
#define REG_OP_MODE     0x4C
#define REG_REP_XY      0x51
#define REG_REP_Z       0x52
#define REG_TRIM_START  0x5D

#define CHIP_ID_EXPECT  0x32

Bmm150::Bmm150(i2c_master_bus_handle_t bus, uint8_t addr)
    : bus_(bus), dev_(nullptr), addr_(addr)
{
    // 清零所有校准字段 (dig_x1_ ~ dig_xy2_)
    memset(&dig_x1_, 0, offsetof(Bmm150, dig_xy2_) - offsetof(Bmm150, dig_x1_) + sizeof(dig_xy2_));
}

Bmm150::~Bmm150() {
    if (dev_) i2c_master_bus_rm_device(dev_);
}

uint8_t Bmm150::ReadReg(uint8_t reg) {
    uint8_t val = 0;
    esp_err_t err = i2c_master_transmit_receive(dev_, &reg, 1, &val, 1, 100);
    if (err != ESP_OK) ESP_LOGW(TAG, "Read 0x%02X failed: %d", reg, err);
    return val;
}

void Bmm150::WriteReg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_master_transmit(dev_, buf, 2, 100);
}

void Bmm150::ReadMulti(uint8_t reg, uint8_t* data, uint8_t len) {
    i2c_master_transmit_receive(dev_, &reg, 1, data, len, 100);
}

bool Bmm150::Init() {
    // 创建 I2C 设备
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr_,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed");
        return false;
    }

    // 验证 Chip ID
    uint8_t id = ReadReg(REG_CHIP_ID);
    if (id != CHIP_ID_EXPECT) {
        ESP_LOGE(TAG, "Bad chip ID: 0x%02X (expected 0x%02X)", id, CHIP_ID_EXPECT);
        return false;
    }
    ESP_LOGI(TAG, "Chip ID OK");

    // 上电
    WriteReg(REG_POWER_CTRL, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 读取修正常数
    if (!ReadTrim()) {
        ESP_LOGE(TAG, "Trim read failed");
        return false;
    }

    // 配置 ODR=30Hz, 正常模式
    WriteReg(REG_OP_MODE, 0x18);     // ODR 30Hz + Normal mode
    WriteReg(REG_REP_XY, 0x04);      // 9 repetitions XY
    WriteReg(REG_REP_Z,  0x10);      // 16 repetitions Z

    ESP_LOGI(TAG, "Init OK");
    return true;
}

bool Bmm150::ReadTrim() {
    uint8_t trim[11];
    ReadMulti(REG_TRIM_START, trim, 11);

    dig_x1_   = (int8_t)trim[0];
    dig_y1_   = (int8_t)trim[1];
    dig_x2_   = (int8_t)trim[2];
    dig_y2_   = (int8_t)trim[3];
    dig_z1_   = (trim[4] & 0x7F);
    dig_z2_   = (int8_t)trim[5];
    dig_z3_   = (int8_t)trim[6];
    dig_z4_   = (int8_t)trim[7];
    dig_xy1_  = trim[8];
    dig_xy2_  = (int8_t)trim[9];
    dig_xyz1_ = (trim[10] & 0x7F);

    ESP_LOGI(TAG, "Trim x1=%d y1=%d x2=%d y2=%d xy1=%u xy2=%d xyz1=%u",
        dig_x1_, dig_y1_, dig_x2_, dig_y2_, dig_xy1_, dig_xy2_, dig_xyz1_);
    return true;
}

bool Bmm150::ReadRaw(int16_t& x, int16_t& y, int16_t& z) {
    uint8_t data[8];
    ReadMulti(REG_DATA_X_LSB, data, 8);

    // 13-bit 数据，右移 3 位
    x = ((int16_t)((data[1] << 8) | data[0])) >> 3;
    y = ((int16_t)((data[3] << 8) | data[2])) >> 3;
    z = ((int16_t)((data[5] << 8) | data[4])) >> 3;

    // 检查 hall 电阻状态
    uint8_t hall = data[6] >> 6;
    if (hall != 1) return false;  // 数据不稳定
    return true;
}

void Bmm150::Compensate(int16_t raw_x, int16_t raw_y, int16_t raw_z,
                         float& comp_x, float& comp_y) {
    // BMM150 简化补偿（Bosch 数据手册公式）
    // 处理溢出
    float temp_x = (float)raw_x;
    float temp_y = (float)raw_y;

    if (temp_x != -4096) temp_x += (dig_xyz1_ * 16384.0f) / 16384.0f;
    if (temp_y != -4096) temp_y += (dig_xyz1_ * 16384.0f) / 16384.0f;

    float gx = 0, gy = 0;
    if (dig_xy2_ != 0) {
        gx = (int16_t)(temp_x - 0) * 16384.0f / (float)(dig_xy2_ + 128);
        gy = (int16_t)(temp_y - 0) * 16384.0f / (float)(dig_xy2_ + 128);
    }

    comp_x = gx * ((dig_x2_ + 128) / 16384.0f) + (dig_x1_ * 32.0f);
    comp_y = gy * ((dig_y2_ + 128) / 16384.0f) + (dig_y1_ * 32.0f);
}

float Bmm150::GetHeading() {
    int16_t rx, ry, rz;
    if (!ReadRaw(rx, ry, rz)) return -1;

    float cx, cy;
    Compensate(rx, ry, rz, cx, cy);

    // 方位角: 0=N, 90=E, 180=S, 270=W
    float heading = atan2f(cy, cx) * 180.0f / M_PI;
    if (heading < 0) heading += 360.0f;

    return heading;
}
