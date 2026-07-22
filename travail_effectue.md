**Infos générales**

- **Projet**: ESP-IDF lisant un capteur BME688 et utilisant BSEC pour piloter les mesures et générer des sorties brutes.
- **Plateforme**: ESP32-S3 XIAO Sense (ESP-IDF).
- **Composants**: capteur Bosch BME68x/BME688, interface I2C, BSEC, carte SD en SPI.

**Structure du projet**

- **Système de build**: `CMakeLists.txt` à la racine.
- **Fichier principal**: `main/main.c` contient l'initialisation I2C, le setup BME68x/BSEC, la gestion SD et la boucle BSEC.
- **Fichiers de config**: `sdkconfig` à la racine.

**Contenu réel de `main.c`**

- Initialise l'I2C sur SDA = GPIO 5, SCL = GPIO 6, port `I2C_NUM_0`, adresse du capteur `0x77`.
- Implémente les callbacks BME68x:
  - `bme68x_i2c_read(...)` avec `i2c_master_transmit_receive()`.
  - `bme68x_i2c_write(...)` avec `i2c_master_transmit()` sur un buffer `reg_addr + payload`.
  - `bme68x_delay_us(...)` avec `esp_rom_delay_us()`.
- `i2c_init()` crée le bus I2C et ajoute le device.

**Gestion de la carte SD**

- Initialise SPI2 pour la carte SD en mode SDSPI avec:
  - MOSI = GPIO 9
  - MISO = GPIO 8
  - CLK = GPIO 7
  - CS = GPIO 43
- Monte le système de fichiers FAT sur `/sdcard`.
- Crée le fichier `/sdcard/bme_log.csv` si nécessaire et écrit l'entête:
  - `sample_index,timestamp_since_poweron_ms,real_time_clock,temperature,pressure,humidity,gas_resistance,heater_profile_step_index,gas_valid,heater_stable,error_code`
- Si la SD n'est pas disponible, l'application continue sans journalisation SD.

**Initialisation BME68x**

- Configure `struct bme68x_dev bme` pour l'interface I2C et les callbacks.
- Définit `amb_temp = 25`.
- Appelle `bme68x_init(&bme)` et affiche `chip_id` si réussi.

**Configuration BSEC**

- Récupère et affiche la version BSEC.
- Appelle `bsec_init()`.
- Souscrit aux sorties brutes suivantes avec `BSEC_SAMPLE_RATE_LP`:
  - `BSEC_OUTPUT_RAW_TEMPERATURE`
  - `BSEC_OUTPUT_RAW_PRESSURE`
  - `BSEC_OUTPUT_RAW_HUMIDITY`
  - `BSEC_OUTPUT_RAW_GAS`
- Appelle `bsec_update_subscription(...)` pour obtenir les réglages physiques requis.

**Boucle principale (`bsec_loop`)**

- Mesure le temps avec `esp_timer_get_time()` et le convertit en nanosecondes.
- Appelle `bsec_sensor_control()` pour récupérer les consignes de mesure.
- Si `sensor_settings.trigger_measurement` est vrai:
  - configure le profil de chauffage avec `bme68x_set_heatr_conf(...)`.
  - configure l'oversampling température/pression/humidité avec `bme68x_set_conf(...)`.
  - met le capteur en mode forcé `BME68X_FORCED_MODE`.
  - attend la durée de mesure et de chauffage.
  - lit les données via `bme68x_get_data(...)`.
- Lors de données valides, enregistre un échantillon dans le fichier CSV SD.
- Construit les entrées BSEC à partir des données valides pour:
  - température
  - humidité
  - pression
  - résistance gaz (si valide)
- Appelle `bsec_do_steps(...)` et affiche les sorties brutes BSEC pour chaque capteur.
- Attend jusqu'au prochain appel demandé par BSEC, ou 10 ms en cas de dépassement.

**Appel principal**

- `app_main()` exécute dans l'ordre:
  - `i2c_init()`
  - `sd_init()`
  - `bme68x_hw_init()`
  - `bsec_setup()`
  - `bsec_loop()`
- Si l'initialisation de la SD échoue, le log SD est désactivé mais l'application continue.
- Si l'initialisation BME688 ou BSEC échoue, l'application s'arrête.

**Observations importantes**

- La boucle est pilotée par BSEC, qui décide du chauffage, du mode et de la cadence des mesures.
- Le code journalise les mesures brutes avant traitement BSEC dans un CSV sur la carte SD.
- Le traitement BSEC est limité aux sorties brutes et ne calcule pas d'IAQ ni d'indicateur de qualité d'air supplémentaire.
- Le script csv_to_bmerawdata permet de convertir le csv de la carte SD en format .bmerawdata (JSON avec un header)

```Shell
python csv_to_bmerawdata.py bme_log_copy.csv session1_fixed_v2.bmerawdata
```
