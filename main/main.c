#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"   
#include "esp_timer.h"    
#include "bme68x.h"       
#include "bsec_interface.h"
#include "bsec_datatypes.h"

static const char *TAG = "NOSE";

// Handle global du device I2C, utilisé par les fonctions de lecture/écriture
static i2c_master_dev_handle_t dev_handle;

// Structure globale du capteur BME68x, utilisée dans la boucle BSEC
static struct bme68x_dev bme;

// Fonctions "glue" entre l'API Bosch et l'I2C ESP-IDF

BME68X_INTF_RET_TYPE bme68x_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                       uint32_t len, void *intf_ptr)
{
    return i2c_master_transmit_receive(dev_handle, &reg_addr, 1, reg_data, len, -1);
}

BME68X_INTF_RET_TYPE bme68x_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                        uint32_t len, void *intf_ptr)
{
    uint8_t buf[len + 1];
    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);
    return i2c_master_transmit(dev_handle, buf, len + 1, -1);
}

void bme68x_delay_us(uint32_t period, void *intf_ptr)
{
    esp_rom_delay_us(period);
}

// Init I2C (bus + device)
static void i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .sda_io_num = 5,
        .scl_io_num = 6,
        .i2c_port = I2C_NUM_0,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus_handle;
    i2c_new_master_bus(&bus_cfg, &bus_handle);

    i2c_device_config_t dev_cfg = {
        .device_address = 0x77,
        .scl_speed_hz = 100000,
    };
    i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
}

// Init BME68x (juste la structure)
static bool bme68x_hw_init(void)
{
    bme.intf = BME68X_I2C_INTF;
    bme.read = bme68x_i2c_read;
    bme.write = bme68x_i2c_write;
    bme.delay_us = bme68x_delay_us;
    bme.intf_ptr = NULL;
    bme.amb_temp = 25;

    int8_t rslt = bme68x_init(&bme);
    if (rslt != BME68X_OK) {
        printf("Erreur bme68x_init: %d\n", rslt);
        return false;
    }

    printf("BME688 initialise avec succes, chip_id = 0x%X\n", bme.chip_id);
    return true;
}

// Init BSEC
static bool bsec_setup(void)
{
    bsec_version_t version;
    bsec_get_version(&version);
    printf("BSEC version: %d.%d.%d.%d\n", version.major, version.minor,
           version.major_bugfix, version.minor_bugfix);

    bsec_library_return_t bsec_status = bsec_init();
    if (bsec_status != BSEC_OK) {
        printf("Erreur bsec_init: %d\n", bsec_status);
        return false;
    }

    // Sorties demandees brutes
    bsec_sensor_configuration_t requested_virtual_sensors[4];
    requested_virtual_sensors[0].sensor_id = BSEC_OUTPUT_RAW_TEMPERATURE;
    requested_virtual_sensors[0].sample_rate = BSEC_SAMPLE_RATE_LP;
    requested_virtual_sensors[1].sensor_id = BSEC_OUTPUT_RAW_PRESSURE;
    requested_virtual_sensors[1].sample_rate = BSEC_SAMPLE_RATE_LP;
    requested_virtual_sensors[2].sensor_id = BSEC_OUTPUT_RAW_HUMIDITY;
    requested_virtual_sensors[2].sample_rate = BSEC_SAMPLE_RATE_LP;
    requested_virtual_sensors[3].sensor_id = BSEC_OUTPUT_RAW_GAS;
    requested_virtual_sensors[3].sample_rate = BSEC_SAMPLE_RATE_LP;

    bsec_sensor_configuration_t required_sensor_settings[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t n_required_sensor_settings = BSEC_MAX_PHYSICAL_SENSOR;

    bsec_status = bsec_update_subscription(requested_virtual_sensors, 4,
                                            required_sensor_settings,
                                            &n_required_sensor_settings);
    if (bsec_status < BSEC_OK) {
        printf("Erreur bsec_update_subscription: %d\n", bsec_status);
        return false;
    } else if (bsec_status != BSEC_OK) { // si ce warning s'affiche c ok 
        printf("Avertissement bsec_update_subscription: %d\n", bsec_status);
    }

    return true;
}

//Boucle principale pilotee par BSEC
static void bsec_loop(void)
{
    while (1) {
        int64_t timestamp_ns = esp_timer_get_time() * 1000; // us -> ns

        bsec_bme_settings_t sensor_settings;
        bsec_sensor_control(timestamp_ns, &sensor_settings);

        if (sensor_settings.trigger_measurement) {
            // Applique le profil de chauffe demande par BSEC
            struct bme68x_heatr_conf heatr_conf;
            heatr_conf.enable = sensor_settings.run_gas ? BME68X_ENABLE : BME68X_DISABLE;
            heatr_conf.heatr_temp = sensor_settings.heater_temperature;
            heatr_conf.heatr_dur = sensor_settings.heater_duration;
            bme68x_set_heatr_conf(BME68X_FORCED_MODE, &heatr_conf, &bme);

            // Applique l'oversampling demande par BSEC
            struct bme68x_conf conf;
            bme68x_get_conf(&conf, &bme);
            conf.os_temp = sensor_settings.temperature_oversampling;
            conf.os_pres = sensor_settings.pressure_oversampling;
            conf.os_hum  = sensor_settings.humidity_oversampling;
            bme68x_set_conf(&conf, &bme);

            bme68x_set_op_mode(sensor_settings.op_mode, &bme);

            uint32_t del_period = bme68x_get_meas_dur(sensor_settings.op_mode, &conf, &bme)
                                   + (heatr_conf.heatr_dur * 1000);
            bme.delay_us(del_period, bme.intf_ptr);

            struct bme68x_data data;
            uint8_t n_fields;
            int8_t rslt = bme68x_get_data(sensor_settings.op_mode, &data, &n_fields, &bme);

            if (rslt == BME68X_OK && n_fields) {
                // Construction des inputs pour BSEC
                bsec_input_t inputs[4];
                uint8_t n_inputs = 0;

                if (sensor_settings.process_data & BSEC_PROCESS_TEMPERATURE) {
                    inputs[n_inputs].sensor_id = BSEC_INPUT_TEMPERATURE;
                    inputs[n_inputs].signal = data.temperature;
                    inputs[n_inputs].time_stamp = timestamp_ns;
                    n_inputs++;
                }
                if (sensor_settings.process_data & BSEC_PROCESS_HUMIDITY) {
                    inputs[n_inputs].sensor_id = BSEC_INPUT_HUMIDITY;
                    inputs[n_inputs].signal = data.humidity;
                    inputs[n_inputs].time_stamp = timestamp_ns;
                    n_inputs++;
                }
                if (sensor_settings.process_data & BSEC_PROCESS_PRESSURE) {
                    inputs[n_inputs].sensor_id = BSEC_INPUT_PRESSURE;
                    inputs[n_inputs].signal = data.pressure;
                    inputs[n_inputs].time_stamp = timestamp_ns;
                    n_inputs++;
                }
                if ((sensor_settings.process_data & BSEC_PROCESS_GAS) &&
                    (data.status & BME68X_GASM_VALID_MSK)) {
                    inputs[n_inputs].sensor_id = BSEC_INPUT_GASRESISTOR;
                    inputs[n_inputs].signal = data.gas_resistance;
                    inputs[n_inputs].time_stamp = timestamp_ns;
                    n_inputs++;
                }

                if (n_inputs > 0) {
                    bsec_output_t outputs[4];
                    uint8_t n_outputs = 4;
                    bsec_library_return_t bsec_status = bsec_do_steps(inputs, n_inputs, outputs, &n_outputs);

                    if (bsec_status == BSEC_OK) {
                        for (uint8_t i = 0; i < n_outputs; i++) {
                            switch (outputs[i].sensor_id) {
                                case BSEC_OUTPUT_RAW_TEMPERATURE:
                                    printf("Temp: %.2f C\n", outputs[i].signal);
                                    break;
                                case BSEC_OUTPUT_RAW_HUMIDITY:
                                    printf("Hum: %.2f %%\n", outputs[i].signal);
                                    break;
                                case BSEC_OUTPUT_RAW_PRESSURE:
                                    printf("Press: %.2f hPa\n", outputs[i].signal / 100.0);
                                    break;
                                case BSEC_OUTPUT_RAW_GAS:
                                    printf("Gas resistance: %.2f Ohm\n", outputs[i].signal);
                                    break;
                                default:
                                    break;
                            }
                        }
                    } else {
                        printf("Erreur bsec_do_steps: %d\n", bsec_status);
                    }
                }
            } else {
                printf("Pas de nouvelles donnees (rslt=%d)\n", rslt);
            }
        }

        // Attendre jusqu'au prochain appel demande par BSEC
        int64_t now_ns = esp_timer_get_time() * 1000;
        int64_t sleep_us = (sensor_settings.next_call - now_ns) / 1000;
        if (sleep_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10)); // securite pour ne pas boucler a vide
        }
    }
}

void app_main(void)
{
    i2c_init();

    if (!bme68x_hw_init()) {
        printf("Arret : BME688 non detecte\n");
        return;
    }

    if (!bsec_setup()) {
        printf("Arret : echec de l'initialisation BSEC\n");
        return;
    }

    bsec_loop();
}