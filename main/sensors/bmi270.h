#ifndef BMI270_H
#define BMI270_H

#include "i2c_device.h"

#include <functional>

class BMI270 : public I2cDevice {
public:
    BMI270(i2c_master_bus_handle_t bus, uint8_t addr = 0x68);
    bool Init();
    void Start();
    void OnShake(std::function<void()> callback);
private:
    uint8_t addr_;
    std::function<void()> shake_callback_;
    static void Task(void* arg);
};

#endif // BMI270_H
