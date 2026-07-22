#!/usr/bin/env python3
"""
Convertit le CSV (bme_log.csv) en un fichier .bmerawdata (JSON) importable dans BME AI-Studio
"""

# un seul capteur / sensor_index = 0
# c pa le kit bosch

# pour lancer
# python csv_to_bmerawdata.py bme_log.csv sortie.bmerawdata

import csv
import json
import sys
import time
import uuid


def convert(csv_path: str, out_path: str, board_id: str = "ESP32S3_XIAO_BME688"):
    rows = []
    with open(csv_path, newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append(r)

    if not rows:
        raise ValueError("Le fichier CSV est vide")

    now = int(time.time())
    now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))

    # reconstruction de la timeline Unix a partir du dernier échantillon
    last_ts_ms = int(rows[-1]["timestamp_since_poweron_ms"])
    for r in rows:
        delta_s = (last_ts_ms - int(r["timestamp_since_poweron_ms"])) / 1000.0
        r["real_time_clock"] = int(now - delta_s)

    data_block = []
    for r in rows:
        data_block.append([
            0, # sensor_index
            1,# sensor_id
            int(r["timestamp_since_poweron_ms"]),
            r["real_time_clock"],
            float(r["temperature"]),
            float(r["pressure"]),
            float(r["humidity"]),
            float(r["gas_resistance"]),
            int(r["heater_profile_step_index"]),
            1, 
            0,    
            0,                                    
            int(r["error_code"]),
        ])

    bmerawdata = {
        "configHeader": {
            "dateCreated_ISO": now_iso,
            "appVersion": "2.2.0",
            "boardType": "board_8",
            "boardMode": "heater_profile_exploration",
            "boardLayout": "grouped",
        },
        "configBody": {
            "heaterProfiles": [
                {
                    "id": "custom_bsec_dynamic",
                    "timeBase": 140,
                    "temperatureTimeVectors": [[300, 14]] * 10,
                }
            ],
            "dutyCycleProfiles": [
                {"id": "duty_1", "numberScanningCycles": 1, "numberSleepingCycles": 0}
            ],
            "sensorConfigurations": [
                {
                    "sensorIndex": 0,
                    "heaterProfile": "custom_bsec_dynamic",
                    "dutyCycleProfile": "duty_1",
                }
            ],
        },
        "rawDataHeader": {
            "counterPowerOnOff": 1,
            "seedPowerOnOff": uuid.uuid4().hex[:16],
            "counterFileLimit": 1,
            "dateCreated": now,
            "dateCreated_ISO": now_iso,
            "firmwareVersion": "0.1.0",
            "boardId": board_id,
        },
        "rawDataBody": {
            "dataColumns": [
                {"name": "Sensor Index", "unit": "", "format": "integer", "key": "sensor_index", "colId": 1},
                {"name": "Sensor ID", "unit": "", "format": "integer", "key": "sensor_id", "colId": 2},
                {"name": "Time Since PowerOn", "unit": "Milliseconds", "format": "integer", "key": "timestamp_since_poweron", "colId": 3},
                {"name": "Real time clock", "unit": "Unix Timestamp: seconds since Jan 01 1970. (UTC); 0 = missing", "format": "integer", "key": "real_time_clock", "colId": 4},
                {"name": "Temperature", "unit": "DegreesCelcius", "format": "float", "key": "temperature", "colId": 5},
                {"name": "Pressure", "unit": "Hectopascals", "format": "float", "key": "pressure", "colId": 6},
                {"name": "Relative Humidity", "unit": "Percent", "format": "float", "key": "relative_humidity", "colId": 7},
                {"name": "Resistance Gassensor", "unit": "Ohms", "format": "float", "key": "resistance_gassensor", "colId": 8},
                {"name": "Heater Profile Step Index", "unit": "", "format": "integer", "key": "heater_profile_step_index", "colId": 9},
                {"name": "Scanning Mode Enabled", "unit": "", "format": "boolean", "key": "scanning_enabled", "colId": 10},
                {"name": "Scanning Cycle Index", "unit": "", "format": "integer", "key": "scanning_cycle_index", "colId": 11},
                {"name": "Label Tag", "unit": "", "format": "integer", "key": "label_tag", "colId": 12},
                {"name": "Error Code", "unit": "", "format": "integer", "key": "error_code", "colId": 13},
            ],
            "dataBlock": data_block,
        },
    }

    with open(out_path, "w") as f:
        json.dump(bmerawdata, f)

    print(f"OK : {len(data_block)} echantillons ecrits dans {out_path}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python csv_to_bmerawdata.py <entree.csv> <sortie.bmerawdata>")
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])