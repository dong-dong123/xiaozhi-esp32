#ifndef _BMM150_H_
#define _BMM150_H_

#include <driver/i2c_master.h>

class Bmm150 {
public:
    Bmm150(i2c_master_bus_handle_t bus, uint8_t addr = 0x10);
    ~Bmm150();

    bool Init();
    bool ReadRaw(int16_t& x, int16_t& y, int16_t& z);
    float GetHeading();

private:
    i2c_master_bus_handle_t bus_;
    i2c_master_dev_handle_t dev_;
    uint8_t addr_;

    // 修正系数（从芯片读取）
    int8_t dig_x1_, dig_y1_, dig_x2_, dig_y2_;
    uint8_t dig_z1_, dig_xy1_, dig_xyz1_;
    int8_t dig_z2_, dig_z3_, dig_z4_, dig_xy2_;

    bool ReadTrim();
    void Compensate(int16_t raw_x, int16_t raw_y, int16_t raw_z,
                    float& comp_x, float& comp_y);
    uint8_t ReadReg(uint8_t reg);
    void WriteReg(uint8_t reg, uint8_t val);
    void ReadMulti(uint8_t reg, uint8_t* data, uint8_t len);
};

#endif // _BMM150_H_
