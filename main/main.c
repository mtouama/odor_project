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

// --- Ajouts pour la carte SD ---
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char *TAG = "NOSE";

// Handle global du device I2C, utilisé par les fonctions de lecture/écriture
static i2c_master_dev_handle_t dev_handle;

// Structure globale du capteur BME68x, utilisée dans la boucle BSEC
static struct bme68x_dev bme;

// --- Configuration carte SD (SPI) ---
// Pins du slot SD dedie de la XIAO ESP32-S3 Sense (expansion board) :
// SCK=GPIO7, MISO=GPIO8, MOSI=GPIO9, CS=GPIO21 (fixe, documente par Seeed).
// Ces pins sont partagees avec le connecteur SPI generique de la carte :
// si tu utilises aussi du SPI ailleurs dans ton projet, tu ne peux pas
// utiliser les deux en meme temps (voir doc Seeed, pastille J3).
#define SD_PIN_MOSI   9
#define SD_PIN_MISO   8
#define SD_PIN_CLK    7
#define SD_PIN_CS     43
#define SD_MOUNT_POINT "/sdcard"
#define SD_CSV_PATH    SD_MOUNT_POINT "/bme_log.csv"
#define I2C_XFER_TIMEOUT_MS 1000

static sdmmc_card_t *sd_card = NULL;
static uint32_t sample_index = 0;     // compteur global d'echantillons (sensor_index / data_point_id)
static uint8_t  heater_step_cycle = 0; // pour le moment à la mano, PENSER A CHANGER CA

// Init de la carte SD en SPI + montage FATFS + creation de l'entete CSV si besoin
static bool sd_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024
    };

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        printf("Erreur spi_bus_initialize: %d\n", ret);
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = SPI2_HOST;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            printf("Erreur : montage du systeme de fichiers SD echoue\n");
        } else {
            printf("Erreur : carte SD non initialisee (%s)\n", esp_err_to_name(ret));
        }
        return false;
    }

    // Ecrit l'entete CSV uniquement si le fichier n'existe pas encore
    FILE *f = fopen(SD_CSV_PATH, "r");
    if (f == NULL) {
        f = fopen(SD_CSV_PATH, "w");
        if (f == NULL) {
            printf("Erreur : impossible de creer %s\n", SD_CSV_PATH);
            return false;
        }
        fprintf(f,
            "sample_index,timestamp_since_poweron_ms,real_time_clock,"
            "temperature,pressure,humidity,gas_resistance,"
            "heater_profile_step_index,gas_valid,heater_stable,error_code\n");
    }
    fclose(f);

    printf("Carte SD montee, log CSV : %s\n", SD_CSV_PATH);
    return true;
}

// Ajoute une ligne de mesure brute au fichier CSV sur la carte SD
static void sd_log_sample(int64_t timestamp_ns, const struct bme68x_data *data)
{
    FILE *f = fopen(SD_CSV_PATH, "a");
    if (f == NULL) {
        printf("Erreur : impossible d'ouvrir %s en ecriture\n", SD_CSV_PATH);
        return;
    }

    uint8_t gas_valid    = (data->status & BME68X_GASM_VALID_MSK) ? 1 : 0;
    uint8_t heater_stable = (data->status & BME68X_HEAT_STAB_MSK) ? 1 : 0;

    fprintf(f, "%lu,%lld,%d,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d\n",
            (unsigned long)sample_index,
            (long long)(timestamp_ns / 1000000), // ms depuis le boot
            0,                                    // real_time_clock inconnu (pas de RTC/NTP), 0 = missing
            data->temperature,
            data->pressure / 100.0,
            data->humidity,
            data->gas_resistance,
            heater_step_cycle,
            gas_valid,
            heater_stable,
            0);

    fclose(f);

    sample_index++;
    heater_step_cycle = (heater_step_cycle + 1) % 10;
}

// Fonctions "glue" entre l'API Bosch et l'I2C ESP-IDF

BME68X_INTF_RET_TYPE bme68x_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                       uint32_t len, void *intf_ptr)
{
    esp_err_t err = i2c_master_transmit_receive(dev_handle, &reg_addr, 1, reg_data, len, I2C_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("Erreur lecture I2C: %s\n", esp_err_to_name(err));
    }
    return err;
}

BME68X_INTF_RET_TYPE bme68x_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                        uint32_t len, void *intf_ptr)
{
   uint8_t buf[len + 1];
    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);
    esp_err_t err = i2c_master_transmit(dev_handle, buf, len + 1, I2C_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("Erreur ecriture I2C: %s\n", esp_err_to_name(err));
    }
    return err;
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

static int consecutive_i2c_errors = 0;

static void i2c_bus_recover(void)
{
    printf("Tentative de recuperation du bus I2C...\n");
    gpio_set_direction(6, GPIO_MODE_OUTPUT); // SCL = GPIO6
    gpio_set_direction(5, GPIO_MODE_INPUT);  // SDA = GPIO5, en lecture

    for (int i = 0; i < 9; i++) {
        gpio_set_level(6, 0);
        esp_rom_delay_us(5);
        gpio_set_level(6, 1);
        esp_rom_delay_us(5);
        if (gpio_get_level(5)) break; // SDA s'est liberee, plus besoin de continuer
    }

    // Reinitialise le bus I2C proprement apres la recuperation manuelle
    i2c_init();
    bme68x_hw_init();
    consecutive_i2c_errors = 0;
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
                consecutive_i2c_errors = 0;
                // Log de la donnee brute sur la carte SD (avant filtrage BSEC)
                if (sd_card != NULL) {
                    sd_log_sample(timestamp_ns, &data);
                }

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
                    consecutive_i2c_errors++;
                    if (consecutive_i2c_errors >= 5) {
                        i2c_bus_recover();
                    }
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

    if (!sd_init()) {
        printf("Avertissement : SD non disponible, le log sur SD sera desactive\n");
    }

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