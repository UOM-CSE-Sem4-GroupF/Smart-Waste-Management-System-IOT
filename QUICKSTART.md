# SWMS Edge Device - Quick Start Checklist

## Before First Build

- [ ] Copy `config_example.h` to `include/config.h`
- [ ] Edit `config.h` with your WiFi credentials
- [ ] Edit `config.h` with your MQTT broker address
- [ ] Edit `config.h` with your bin ID and zone
- [ ] Calibrate `EMPTY_DISTANCE_MM` and `FULL_DISTANCE_MM` for your bin
- [ ] Verify GPIO pins match your ESP32 wiring

## First Build & Upload

```bash
cd /home/vihanga/Desktop/se_project

# Build
~/.platformio/penv/bin/platformio run

# Upload to board
~/.platformio/penv/bin/platformio run --target upload

# Monitor serial output
~/.platformio/penv/bin/platformio device monitor --baud 115200
```

## Expected Serial Output

```
=== SWMS Edge Device Starting ===

Connecting to WiFi: your_ssid
...
WiFi connected!
IP address: 192.168.x.x
Connecting to MQTT broker: your.broker.com:1883
MQTT connected to your.broker.com:1883

Sensor 1: 125 mm
Sensor 2: 128 mm
Published: sensors/bin/BIN-EDGE-001/telemetry = {...}
Fill: 49.5% | Battery: 92.1% | Next publish in 600s
---
```

## Verify Telemetry Reception

Consume MQTT messages from your broker:

```bash
mosquitto_sub -h your.broker.com -u sensor-device -P "password" \
  -t "sensors/bin/+/telemetry"
```

## Real Data Transition Checklist

### Battery Reading (when hardware ready)

1. Connect battery voltage divider to GPIO 34 (ADC1_CH6)
2. Measure ADC values:
   - At 0% battery: note ADC value → `BATTERY_ADC_MIN`
   - At 100% battery: note ADC value → `BATTERY_ADC_MAX`
3. Update in `config.h`
4. Replace `readBatteryPercentage()` simulation with actual ADC math

### Temperature Sensor (when hardware ready)

1. Add library to `platformio.ini` (DHT22 or BMP280)
2. Connect sensor to GPIO pin of choice
3. Replace `readTemperature()` placeholder function
4. Update GPIO definition in `config.h`

### NTP Time Sync (when internet available)

1. Uncomment NTP code in `setup()`
2. This enables accurate `timestamp` fields in telemetry

## Troubleshooting

**WiFi connects but MQTT fails:**

- Check broker address is resolvable (use IP if DNS unavailable)
- Verify username/password
- Ensure broker allows connections from your IP

**Telemetry not published:**

- Check MQTT topic in serial output
- Verify broker is receiving messages: `mosquitto_sub -v ...`
- Look for "Failed to publish" in serial monitor

**Fill percentage looks wrong:**

- Run this calibration test:
  1. Place full bin in front of sensors
  2. Note distance in serial output (should be near FULL_DISTANCE_MM)
  3. Empty the bin
  4. Note distance (should be near EMPTY_DISTANCE_MM)
  5. Adjust constants and rebuild

## Next Steps

See `IMPLEMENTATION.md` for:

- Full architecture details
- Future enhancements roadmap
- Error flag implementation
- Local buffering strategy
