#ifndef _QMI8658_H_
#define _QMI8658_H_

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct ImuData {
    float acc_x, acc_y, acc_z;   // m/s²
    float gyr_x, gyr_y, gyr_z;   // rad/s
    uint32_t timestamp_ms;
};

class Qmi8658 {
public:
    Qmi8658(i2c_master_bus_handle_t bus, uint8_t addr = 0x6A);
    ~Qmi8658();

    bool Init();
    bool Read(ImuData& data);

private:
    i2c_master_bus_handle_t bus_;
    i2c_master_dev_handle_t dev_;
    uint8_t addr_;
    float acc_scale_, gyr_scale_;

    uint8_t ReadReg(uint8_t reg);
    void WriteReg(uint8_t reg, uint8_t val);
    void ReadMulti(uint8_t reg, uint8_t* data, uint8_t len);
};

// IMU 功能型任务
struct ImuTaskCtx {
    Qmi8658* imu;
    void (*on_step)(void* user, int steps);
    void (*on_wrist_up)(void* user);
    void (*on_shake)(void* user);
    void* user_data;
};

void imu_task(void* arg);

#endif // _QMI8658_H_
