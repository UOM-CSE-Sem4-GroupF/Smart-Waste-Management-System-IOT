// Configuration Template for SWMS Edge Device
// Copy this to config.h and fill in your WiFi and MQTT credentials

#ifndef CONFIG_H
#define CONFIG_H

// ============ WiFi Configuration ============
// Update with your WiFi network credentials
#define WIFI_SSID "your_wifi_network"
#define WIFI_PASSWORD "your_wifi_password"

// ============ MQTT Configuration ============
// Update with your MQTT broker details
#define MQTT_BROKER "emqx.example.com" // or IP address
#define MQTT_PORT 1883
#define MQTT_USER "sensor-device"
#define MQTT_PASSWORD "swms-sensor-dev-2026"
#define MQTT_TOPIC_PREFIX "sensors"

// ============ Bin Configuration ============
// Update based on your bin placement
#define BIN_ID "BIN-EDGE-001"
#define ZONE_ID 1
#define WASTE_CATEGORY "general" // Options: food_waste, general, paper, plastic, glass
#define BIN_VOLUME_LITRES 240

// ============ Sensor Calibration ============
// Calibrate based on your physical bin dimensions
#define EMPTY_DISTANCE_MM 240 // ToF distance reading when bin is empty
#define FULL_DISTANCE_MM 0    // ToF distance reading when bin is full

// ============ GPIO Pins ============
// Adjust if using different pins
#define SENSOR1_XSHUT 16
#define SENSOR2_XSHUT 17
#define SENSOR1_ADDR 0x30
#define SENSOR2_ADDR 0x31
#define LED_PIN 2
#define BATTERY_ADC_PIN 34 // ESP32 ADC pin for battery voltage
#define DHT_PIN 4          // GPIO used for DHT22 data line (change if needed)
#define DHT_TYPE DHT22     // DHT11 or DHT22

// ============ Battery Calibration ============
// Update these values based on your battery module ADC readings
#define BATTERY_ADC_MIN 1240 // ADC value at 0% battery
#define BATTERY_ADC_MAX 4095 // ADC value at 100% battery

// ============ NTP Time Server ============
// For timestamp synchronization
#define NTP_SERVER "pool.ntp.org"
#define UTC_OFFSET 0 // UTC offset in hours (e.g., 5.5 for IST)

#endif // CONFIG_H
