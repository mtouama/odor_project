#include "bme68x.h"
#include "driver/i2c_master.h"

static i2c_master_dev_handle_t dev_handle;

BME68X_INTF_RET_TYPE bme68x_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                       uint32_t len, void *intf_ptr) {
    return i2c_master_transmit_receive(dev_handle, &reg_addr, 1, reg_data, len, -1);
}

BME68X_INTF_RET_TYPE bme68x_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                        uint32_t len, void *intf_ptr) {
    uint8_t buf[len + 1];
    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);
    return i2c_master_transmit(dev_handle, buf, len + 1, -1);
}

void bme68x_delay_us(uint32_t period, void *intf_ptr) {
    esp_rom_delay_us(period);
}