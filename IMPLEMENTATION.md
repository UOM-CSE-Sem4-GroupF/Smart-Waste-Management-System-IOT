# SWMS Edge Device Firmware - Enhanced with MQTT Telemetry

## Overview

This firmware transforms the ESP32 + dual VL53L0X bin sensor into a complete IoT device that:

- **Reads real fill data** from dual ToF sensors
- **Publishes MQTT telemetry** matching the simulator format
- **Implements adaptive sleep cycles** (10-30 sec based on fill level)
- **Measures battery voltage** via ADC (with fallback to simulated data)
- **Supports future temperature sensor** integration
- **Maintains sensor fault recovery** from original design

## Architecture

```
ESP32 + 2x VL53L0X Sensors
        ↓
   Read Distance (mm)
        ↓
Calculate Fill Level %
        ↓
Package MQTT Telemetry
        ↓
MQTT Broker (EMQX)
        ↓
    Kafka Topic: waste.bin.telemetry
```

## Key Features from Simulator

| Feature              | Implementation                                                                                                                                       |
| -------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Telemetry Format** | Exact match: `bin_id`, `fill_level_pct`, `battery_level_pct`, `signal_strength_dbm`, `temperature_c`, `timestamp`, `firmware_version`, `error_flags` |
| **Adaptive Sleep**   | <50%: 10min, 50-75%: 5min, 75-90%: 2min, >90%: 30sec                                                                                                 |
| **Signal Strength**  | WiFi RSSI (real dBm reading)                                                                                                                         |
| **Battery Reading**  | ADC input on pin 34 (configurable)                                                                                                                   |
| **Temperature**      | Placeholder (ready for DHT22/BMP280)                                                                                                                 |
| **Error Flags**      | Placeholder for sensor detection                                                                                                                     |

## Configuration

1. **Copy the template:**

   ```bash
   cp config_example.h include/config.h
   ```

2. **Edit WiFi & MQTT credentials in `include/config.h`:**

   ```cpp
   #define WIFI_SSID "your_network"
   #define MQTT_BROKER "broker.address.com"
   #define MQTT_USER "sensor-device"
   #define MQTT_PASSWORD "your_password"
   ```

3. **Calibrate sensor for your bin:**

   ```cpp
   #define EMPTY_DISTANCE_MM 240    // ToF reading when empty
   #define FULL_DISTANCE_MM 0       // ToF reading when full
   ```

4. **Build and upload:**
   ```bash
   ~/.platformio/penv/bin/platformio run --target upload
   ```

## Telemetry Format

Published to `sensors/bin/BIN-EDGE-001/telemetry`:

```json
{
  "bin_id": "BIN-EDGE-001",
  "fill_level_pct": 67.3,
  "battery_level_pct": 85.4,
  "signal_strength_dbm": -68,
  "temperature_c": 28.5,
  "timestamp": "2026-05-08T14:30:45Z",
  "firmware_version": "2.1.4",
  "error_flags": 0
}
```

## Publishing Intervals

Based on real fill level (adaptive sleep reduces power consumption):

| Fill Level | Interval   | Use Case                            |
| ---------- | ---------- | ----------------------------------- |
| < 50%      | 10 minutes | Normal operation, low power         |
| 50-75%     | 5 minutes  | Moderate fill, increased monitoring |
| 75-90%     | 2 minutes  | High fill, frequent updates         |
| > 90%      | 30 seconds | Critical, urgent action needed      |

## Real vs. Placeholder Data

### ✅ Real Data (from Hardware)

- `fill_level_pct` — from VL53L0X sensors
- `signal_strength_dbm` — WiFi RSSI
- `bin_id`, `zone_id`, `waste_category` — hardcoded configuration

### ⏳ Placeholder (Ready for Implementation)

- `battery_level_pct` — currently simulated, ready for ADC calibration
- `temperature_c` — currently simulated, ready for DHT22/BMP280
- `error_flags` — currently 0, ready for sensor fault detection

## Future Enhancements

1. **Battery ADC Calibration:**
   - Measure voltage divider output at known battery states
   - Update `BATTERY_ADC_MIN` and `BATTERY_ADC_MAX` in config
   - Implement `readBatteryPercentage()` with real ADC

2. **Temperature Sensor:**
   - Add DHT22 or BMP280 library to `platformio.ini`
   - Implement `readTemperature()` function
   - Configure GPIO pin

3. **NTP Time Synchronization:**
   - Use `configTime()` to sync with NTP server
   - Generate accurate `timestamp` fields

4. **Error Detection:**
   - Monitor sensor I2C failures
   - Set error flags (bits: sensor read, lid open, tilt, sunlight)
   - Track and report faults

5. **Local Buffering:**
   - Add SQLite or SPIFFS storage
   - Buffer telemetry during WiFi/MQTT outages
   - Replay on reconnection

## Monitoring

Watch telemetry in real-time:

```bash
# If using kcat with Kafka
kcat -F client.properties -t waste.bin.telemetry -C -o end -q

# Or subscribe directly to MQTT
mosquitto_sub -h broker.address.com -u sensor-device -P "password" \
  -t "sensors/bin/BIN-EDGE-001/telemetry"
```

## Troubleshooting

| Issue                  | Solution                                                    |
| ---------------------- | ----------------------------------------------------------- |
| WiFi not connecting    | Check SSID/password in config.h, verify signal strength     |
| MQTT connection fails  | Verify broker address, port, credentials                    |
| No telemetry published | Check WiFi/MQTT status in serial monitor, verify topic name |
| Wrong fill percentage  | Calibrate `EMPTY_DISTANCE_MM` and `FULL_DISTANCE_MM`        |

## Serial Output Example

```
=== SWMS Edge Device Starting ===

Connecting to WiFi: MyNetwork
........................
WiFi connected!
IP address: 192.168.1.100
Connecting to MQTT broker: emqx.local:1883
MQTT connected to emqx.local:1883

Sensor 1: 145 mm
Sensor 2: 142 mm
Published: sensors/bin/BIN-EDGE-001/telemetry = {"bin_id":"BIN-EDGE-001","fill_level_pct":39.2,...}
Fill: 39.2% | Battery: 87.3% | Next publish in 600s
---
```

## References

- Simulator behavior: `../Smart-Waste-Management-System-Edge/simulator.py`
- Service spec: `../Smart-Waste-Management-System-Edge/service-specifications.md`
- VL53L0X library: https://github.com/pololu/vl53l0x-arduino
- PubSubClient: https://github.com/knolleary/pubsubclient
