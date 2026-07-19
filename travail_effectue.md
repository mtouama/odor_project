**Infos générales**

- **Projet**: ESP-IDF qui lit un capteur BME688 et utilise BSEC pour piloter les mesures.
- **Plateforme**: ESP-IDF (microcontrôleur ESP32S3 XIAO).
- **Composants**: capteur Bosch BME68x/BME688, interface I2C, traitement BSEC.

**Structure du projet**

- **Système de build**: `CMakeLists.txt` à la racine.
- **Fichier principal**: `main.c` contient l'initialisation I2C, le setup BME68x/BSEC et la boucle principale pilotée par BSEC.
- **Fichiers de config**: `sdkconfig` à la racine.

**Composants principaux (/components)**

- **bme68x**: driver Bosch BME68x pour l'accès capteur.
- **bsec2**: composant BSEC pour le pilotage du heater et le traitement des données de capteurs.
- **sdcard_logger**: présent dans le projet, mais non utilisé dans `main.c`.

**Contenu réel de `main.c`**

- Initialise l'I2C sur SDA = GPIO 5, SCL = GPIO 6, port I2C = `I2C_NUM_0`, adresse du device = `0x77`.
- Implémente les callbacks BME68x:
  - `bme68x_i2c_read(...)` utilise `i2c_master_transmit_receive`.
  - `bme68x_i2c_write(...)` envoie un buffer `[addr + payload]` via `i2c_master_transmit`.
  - `bme68x_delay_us(...)` appelle `esp_rom_delay_us`.
- `i2c_init()` crée le bus I2C et ajoute le device.

**Initialisation BME68x**

- Remplit `struct bme68x_dev bme` avec `intf = BME68X_I2C_INTF`, les callbacks et `amb_temp = 25`.
- Appelle `bme68x_init(&bme)` et vérifie le retour.
- Affiche le `chip_id` si l'initialisation est réussie.

**Configuration BSEC**

- Récupère la version BSEC et l'affiche.
- Appelle `bsec_init()`.
- Demande les sorties brutes suivantes en basse fréquence:
  - `BSEC_OUTPUT_RAW_TEMPERATURE`
  - `BSEC_OUTPUT_RAW_PRESSURE`
  - `BSEC_OUTPUT_RAW_HUMIDITY`
  - `BSEC_OUTPUT_RAW_GAS`
- Met à jour la souscription BSEC avec `bsec_update_subscription(...)`.

**Boucle principale (`bsec_loop`)**

- Appelle `bsec_sensor_control(timestamp_ns, &sensor_settings)` pour obtenir les consignes BSEC.
- Si `sensor_settings.trigger_measurement` est vrai:
  - configure le chauffage avec `bme68x_set_heatr_conf(...)` selon BSEC.
  - configure les oversamplings température/pression/humidité selon BSEC.
  - met le capteur en mode forcé `BME68X_FORCED_MODE`.
  - attend la durée de mesure + la durée de chauffage.
  - lit les données avec `bme68x_get_data(...)`.
- Si les données sont valides, construit les entrées BSEC pour:
  - la température
  - l'humidité
  - la pression
  - la résistance gaz (si valide)
- Appelle `bsec_do_steps(...)` et affiche les sorties BSEC lues.
- Utilise `vTaskDelay(...)` pour attendre jusqu'au prochain appel demandé par BSEC, avec un fallback de 10 ms.

**Appel principal**

- `app_main()` exécute:
  - `i2c_init()`
  - `bme68x_hw_init()`
  - `bsec_setup()`
  - `bsec_loop()`

**Observations importantes**

- Le code actuel n'enregistre pas de données sur carte SD; il affiche uniquement les valeurs sur la console.
- La gestion du chauffage est déléguée à BSEC et appliquée dans la boucle de mesure.
- Les entrées envoyées à BSEC proviennent des lectures valides du BME688.

**À surveiller / améliorations possibles**

- Intégrer `sdcard_logger` pour sauvegarder les mesures.
- Ajouter un traitement additionnel des sorties BSEC (IAQ, indice qualité d'air) si besoin
