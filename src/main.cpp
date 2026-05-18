#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_system.h>
#include <DHT.h>
#include <INA226_WE.h>
// Try to include user config if present
#if defined(__has_include)
#if __has_include("config.h")
#include "config.h"
#endif
#endif

// ============ GPIO & Sensor Configuration ============
constexpr uint8_t SENSOR1_XSHUT = 18;
constexpr uint8_t SENSOR2_XSHUT = 19;
constexpr uint8_t SENSOR1_ADDR = 0x30;
constexpr uint8_t SENSOR2_ADDR = 0x31;
constexpr uint8_t LED_PIN = 2;

// ============ INA226 Configuration ============
constexpr uint8_t INA226_I2C_ADDR = 0x40;  // A0=GND, A1=GND (default)
constexpr float SHUNT_RESISTOR_OHM = 0.1f; // 100 mΩ shunt resistor
constexpr float MAX_EXPECTED_CURRENT_A = 1.0f;
constexpr float BATTERY_FULL_V = 8.4f;  // 3.7V LiPo fully charged
constexpr float BATTERY_EMPTY_V = 6.0f; // cut-off voltage
#ifndef DHT_PIN
constexpr uint8_t DHT_SENSOR_PIN = 4; // fallback pin if not provided by config.h
#else
constexpr uint8_t DHT_SENSOR_PIN = DHT_PIN;
#endif

#ifndef DHT_TYPE
#define DHT_TYPE DHT22
#endif

DHT dht(DHT_SENSOR_PIN, DHT_TYPE);

// ============ Sensor Recovery Configuration ============
constexpr int MAX_RECOVER_ATTEMPTS = 5;
constexpr int RECOVER_DELAY_MS = 200;
constexpr int MAX_CONSECUTIVE_FAILURES = 3;

// ============ Bin Configuration ============
constexpr const char *BIN_ID = "BIN-011";
constexpr uint8_t ZONE_ID = 1;
constexpr const char *WASTE_CATEGORY = "general"; // food_waste, general, paper, plastic, glass
constexpr uint16_t BIN_VOLUME_LITRES = 240;
constexpr const char *FIRMWARE_VERSION = "2.1.4";

// ============ WiFi & MQTT Configuration ============
const char *WIFI_SSID = "ZTE Blade V50 Design";
const char *WIFI_PASSWORD = "102938asdf";
const char *MQTT_BROKER = "167.99.28.170"; // TODO: Configure MQTT
constexpr uint16_t MQTT_PORT = 1883;
const char *MQTT_USER = "sensor-device";       // TODO: Configure MQTT user
const char *MQTT_PASSWORD = "swms-sensor-dev-2026"; // TODO: Configure MQTT password
const char *MQTT_TOPIC_PREFIX = "sensors";

// ============ Sensor Objects ============
VL53L0X sensor1;
VL53L0X sensor2;
INA226_WE ina226(INA226_I2C_ADDR);

// ============ Sensor State ============
int sensor1Failures = 0;
int sensor2Failures = 0;

// ============ WiFi & MQTT Objects ============
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ============ Telemetry State ============
float currentFillPercentage = 0.0;
float batteryPercentage = 100.0;
float batteryVoltageV = 3.7f;
float batteryCurrentMa = 0.0f;
int signalStrengthDbm = -70;
float temperatureCelsius = 25.0;

// ============ LED Pattern Manager ============
enum LedPattern : uint8_t {
  LED_OFF = 0,
  LED_HEARTBEAT,
  LED_WIFI_CONNECTED,
  LED_MQTT_CONNECTED,
  LED_SUCCESS,
  LED_ERROR,
  LED_PUBLISH_OK,
  LED_PUBLISH_FAIL,
  LED_SENSOR_RECOVERING,
};

// Patterns expressed as alternating ON/OFF durations (ms). Last element 0 marks end.
static const uint32_t PAT_HEARTBEAT[] = {150, 850, 0};          // short on, long off
static const uint32_t PAT_WIFI[] = {200, 200, 200, 800, 0};     // double blink
static const uint32_t PAT_MQTT[] = {100, 100, 100, 100, 100, 700, 0}; // triple quick
static const uint32_t PAT_SUCCESS[] = {80, 80, 80, 600, 0};     // two short blinks
static const uint32_t PAT_ERROR[] = {120, 120, 120, 120, 0};    // rapid blink
static const uint32_t PAT_PUB_OK[] = {60, 240, 0};              // single short blink
static const uint32_t PAT_PUB_FAIL[] = {400, 400, 0};           // long blink
static const uint32_t PAT_RECOVER[] = {80, 80, 80, 80, 400, 0}; // busy blink

uint8_t currentLedPattern = LED_OFF;
const uint32_t *currentPatternPtr = nullptr;
uint8_t currentPatternStep = 0; // index into pattern (counts ON/OFF pairs)
uint32_t ledLastToggleMs = 0;
bool ledIsOn = false;
bool currentPatternRepeat = true;
LedPattern fallbackLedPattern = LED_HEARTBEAT;

void setLedPattern(LedPattern p, bool repeat = true, LedPattern fallback = LED_HEARTBEAT)
{
  currentPatternRepeat = repeat;
  fallbackLedPattern = fallback;
  currentLedPattern = (uint8_t)p;
  currentPatternStep = 0;
  ledLastToggleMs = millis();
  ledIsOn = false;
  switch (p)
  {
  case LED_HEARTBEAT:
    currentPatternPtr = PAT_HEARTBEAT;
    break;
  case LED_WIFI_CONNECTED:
    currentPatternPtr = PAT_WIFI;
    break;
  case LED_MQTT_CONNECTED:
    currentPatternPtr = PAT_MQTT;
    break;
  case LED_SUCCESS:
    currentPatternPtr = PAT_SUCCESS;
    break;
  case LED_ERROR:
    currentPatternPtr = PAT_ERROR;
    break;
  case LED_PUBLISH_OK:
    currentPatternPtr = PAT_PUB_OK;
    break;
  case LED_PUBLISH_FAIL:
    currentPatternPtr = PAT_PUB_FAIL;
    break;
  case LED_SENSOR_RECOVERING:
    currentPatternPtr = PAT_RECOVER;
    break;
  default:
    currentPatternPtr = nullptr;
    break;
  }
  digitalWrite(LED_PIN, LOW);
}

void updateLed()
{
  if (!currentPatternPtr)
  {
    digitalWrite(LED_PIN, LOW);
    return;
  }

  uint32_t now = millis();
  // Find the duration for current step
  uint8_t idx = currentPatternStep;
  uint32_t dur = currentPatternPtr[idx];
  if (dur == 0)
  {
    // pattern ended; wrap to start
    currentPatternStep = 0;
    idx = 0;
    dur = currentPatternPtr[0];
  }

  if ((now - ledLastToggleMs) >= dur)
  {
    // toggle LED and advance
    ledIsOn = !ledIsOn;
    digitalWrite(LED_PIN, ledIsOn ? HIGH : LOW);
    ledLastToggleMs = now;
    currentPatternStep++;
    // if next is 0 we reached end of pattern
    if (currentPatternPtr[currentPatternStep] == 0)
    {
      if (currentPatternRepeat)
      {
        currentPatternStep = 0; // loop
      }
      else
      {
        // one-shot finished — revert to fallback pattern
        setLedPattern(fallbackLedPattern);
      }
    }
  }
}

// Sleep helper that keeps LED updates and MQTT loop alive during long waits
void sleepWithLed(uint32_t ms)
{
  uint32_t start = millis();
  while ((millis() - start) < ms)
  {
    mqttClient.loop();
    updateLed();
    delay(50);
  }
}

// NTP defaults (overridable via include/config.h -> #define NTP_SERVER "..." and #define UTC_OFFSET <hours>)
#ifndef NTP_SERVER
static const char *NTP_SERVER_STR = "pool.ntp.org";
#else
static const char *NTP_SERVER_STR = NTP_SERVER;
#endif

#ifndef UTC_OFFSET
static const double UTC_OFFSET_HOURS = 0.0;
#else
static const double UTC_OFFSET_HOURS = UTC_OFFSET;
#endif

const char *resetReasonToString(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_POWERON:
    return "POWERON";
  case ESP_RST_EXT:
    return "EXT";
  case ESP_RST_SW:
    return "SW";
  case ESP_RST_PANIC:
    return "PANIC";
  case ESP_RST_INT_WDT:
    return "INT_WDT";
  case ESP_RST_TASK_WDT:
    return "TASK_WDT";
  case ESP_RST_WDT:
    return "WDT";
  case ESP_RST_DEEPSLEEP:
    return "DEEPSLEEP";
  case ESP_RST_BROWNOUT:
    return "BROWNOUT";
  case ESP_RST_SDIO:
    return "SDIO";
  default:
    return "UNKNOWN";
  }
}

String getIsoUtcTimestamp()
{
  time_t now = time(nullptr);
  struct tm tmstruct;
  gmtime_r(&now, &tmstruct);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmstruct);
  return String(buf);
}

void syncTimeWithNtp()
{
  long gmtOffsetSec = (long)(UTC_OFFSET_HOURS * 3600);
  configTime(gmtOffsetSec, 0, NTP_SERVER_STR);

  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 24 * 3600 && attempts < 10)
  {
    delay(1000);
    now = time(nullptr);
    attempts++;
  }
  if (now >= 24 * 3600)
  {
    setLedPattern(LED_SUCCESS, false, LED_HEARTBEAT);
  }
}

void resetI2CBus()
{
  Wire.end();
  delay(10);
  Wire.begin();
}

// ============ WiFi Functions ============
void connectToWiFi()
{
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20)
  {
    delay(500);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    setLedPattern(LED_WIFI_CONNECTED);
  }
  else
  {
    setLedPattern(LED_ERROR);
  }
}

void reconnectWiFi()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    connectToWiFi();
  }
}

// ============ MQTT Functions ============
void onMqttConnect()
{
  // indicate MQTT connected
  setLedPattern(LED_MQTT_CONNECTED);
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  (void)topic;
  (void)payload;
  (void)length;
}

void connectToMQTT()
{
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);

  int retries = 0;
  while (!mqttClient.connected() && retries < 5)
  {
    if (mqttClient.connect(BIN_ID, MQTT_USER, MQTT_PASSWORD))
    {
      onMqttConnect();
      return;
    }
    delay(1000);
    retries++;
  }

  if (!mqttClient.connected())
  {
    setLedPattern(LED_ERROR);
  }
}

void reconnectMQTT()
{
  if (!mqttClient.connected())
  {
    connectToMQTT();
  }
}

// ============ Sensor Reading & Telemetry Functions ============
uint16_t getAverageDistance()
{
  // Read both sensors and return average for more stable fill level
  uint16_t dist1 = sensor1.readRangeContinuousMillimeters();
  uint16_t dist2 = sensor2.readRangeContinuousMillimeters();

  // Handle sensor failures (65535 = timeout, 0 = invalid)
  if (dist1 == 65535 || dist1 == 0)
    dist1 = dist2;
  if (dist2 == 65535 || dist2 == 0)
    dist2 = dist1;

  // If both are valid, average them
  if (dist1 != 65535 && dist2 != 65535 && dist1 != 0 && dist2 != 0)
  {
    return (dist1 + dist2) / 2;
  }

  // Return whichever is valid
  return (dist1 != 65535 && dist1 != 0) ? dist1 : dist2;
}

float calculateFillPercentage(uint16_t distanceMm)
{
  // TODO: Calibrate these values based on your bin dimensions
  // For now: assume 0mm (full) = 0% and 350mm (empty) = 100%
  const uint16_t EMPTY_DISTANCE_MM = 350; // distance when bin is empty
  const uint16_t FULL_DISTANCE_MM = 40;    // distance when bin is full

  if (distanceMm >= EMPTY_DISTANCE_MM)
    return 0.0; // Empty
  if (distanceMm <= FULL_DISTANCE_MM)
    return 100.0; // Full

  float fillPct = 100.0 * (EMPTY_DISTANCE_MM - distanceMm) / (EMPTY_DISTANCE_MM - FULL_DISTANCE_MM);
  return constrain(fillPct, 0.0, 100.0);
}

float readBatteryPercentage()
{
  batteryVoltageV = ina226.getBusVoltage_V();
  batteryCurrentMa = ina226.getCurrent_mA();
  float pct = (batteryVoltageV - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100.0f;
  return constrain(pct, 0.0f, 100.0f);
}

float readTemperature()
{
  // Return a synthetic temperature between 30.0 and 40.0 °C
  uint32_t r = esp_random();
  float frac = (float)r / 4294967295.0f; // normalize to [0,1]
  float t = 30.0f + frac * 10.0f;
  temperatureCelsius = t;
  return temperatureCelsius;
}

int getSignalStrength()
{
  // Get WiFi RSSI (signal strength in dBm)
  return WiFi.RSSI();
}

// ============ Adaptive Sleep Cycle ============
uint32_t calculatePublishIntervalMs(float fillPercentage)
{
  // Based on simulator: adaptive sleep reduces power drain when bin is mostly empty
  // Returns milliseconds to sleep before next publish

  // if (fillPercentage < 50.0)
  //   return 60000; // 10 minutes
  // else if (fillPercentage < 75.0)
  //   return 300000; // 5 minutes
  // else if (fillPercentage < 90.0)
  //   return 120000; // 2 minutes
  // else
    return 5000; // 5 seconds (urgent)
}

// ============ Telemetry Publishing ============
void publishTelemetry()
{
  // Read current sensor data
  uint16_t distanceMm = getAverageDistance();
  currentFillPercentage = calculateFillPercentage(distanceMm);
  batteryPercentage = readBatteryPercentage();
  temperatureCelsius = readTemperature();
  signalStrengthDbm = getSignalStrength();

  // Create JSON payload matching simulator format
  DynamicJsonDocument doc(512);
  doc["bin_id"] = BIN_ID;
  doc["fill_level_pct"] = round(currentFillPercentage * 100.0) / 100.0;
  doc["battery_level_pct"] = round(batteryPercentage * 10.0) / 10.0;
  doc["battery_voltage_v"] = round(batteryVoltageV * 100.0) / 100.0;
  doc["battery_current_ma"] = round(batteryCurrentMa * 10.0) / 10.0;
  doc["signal_strength_dbm"] = signalStrengthDbm;
  doc["temperature_c"] = round(temperatureCelsius * 10.0) / 10.0;
  doc["timestamp"] = getIsoUtcTimestamp();

  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["error_flags"] = 0; // TODO: Add error detection logic

  // Serialize and publish
  String payload;
  serializeJson(doc, payload);

  String topic = String(MQTT_TOPIC_PREFIX) + "/bin/" + BIN_ID + "/telemetry";

  if (mqttClient.publish(topic.c_str(), payload.c_str(), true))
  {
    setLedPattern(LED_PUBLISH_OK, false, LED_HEARTBEAT);
  }
  else
  {
    setLedPattern(LED_PUBLISH_FAIL, false, LED_HEARTBEAT);
  }
}

void configureSensor(VL53L0X &sensor, uint8_t xshutPin, uint8_t address)
{
  digitalWrite(xshutPin, LOW);
  delay(10);

  digitalWrite(xshutPin, HIGH);
  delay(10);

  if (!sensor.init())
  {
    while (true)
    {
      delay(100);
    }
  }

  sensor.setAddress(address);
  sensor.setTimeout(500);
  sensor.startContinuous();
}

bool recoverSensor(VL53L0X &sensor, uint8_t xshutPin, uint8_t address)
{
  setLedPattern(LED_SENSOR_RECOVERING);
  for (int attempt = 1; attempt <= MAX_RECOVER_ATTEMPTS; ++attempt)
  {
    digitalWrite(LED_PIN, HIGH);

    // Reset the I2C bus and force this sensor back through startup.
    resetI2CBus();

    digitalWrite(SENSOR1_XSHUT, LOW);
    digitalWrite(SENSOR2_XSHUT, LOW);
    delay(10);

    digitalWrite(xshutPin, HIGH);
    delay(50);

    // Toggle XSHUT to reset the sensor
    digitalWrite(xshutPin, LOW);
    delay(10);
    digitalWrite(xshutPin, HIGH);
    delay(50);

    // Try to (re)initialize
    if (!sensor.init())
    {
      digitalWrite(LED_PIN, LOW);
      delay(RECOVER_DELAY_MS);
      continue;
    }

    sensor.setAddress(address);
    sensor.setTimeout(500);
    sensor.startContinuous();

    // quick read to see if sensor responds
    uint16_t d = sensor.readRangeContinuousMillimeters();
    if (!sensor.timeoutOccurred() && d != 65535)
    {
        setLedPattern(LED_SUCCESS, false, LED_HEARTBEAT);
      return true;
    }

    digitalWrite(LED_PIN, LOW);
    delay(RECOVER_DELAY_MS);
  }

  setLedPattern(LED_ERROR);
  return false;
}

bool isSensorReadingValid(VL53L0X &sensor, uint16_t distanceMm)
{
  return !sensor.timeoutOccurred() && distanceMm != 65535;
}

void printAndMaybeRecover(VL53L0X &sensor, uint8_t xshutPin, uint8_t address, const char *label, int &failureCount)
{
  uint16_t distance = sensor.readRangeContinuousMillimeters();
  if (isSensorReadingValid(sensor, distance))
  {
    failureCount = 0;
    return;
  }

  ++failureCount;
  if (failureCount < MAX_CONSECUTIVE_FAILURES)
  {
    return;
  }

  if (recoverSensor(sensor, xshutPin, address))
  {
    failureCount = 0;
  }
}

void setup()
{
  delay(1000);

  Wire.begin();

  pinMode(SENSOR1_XSHUT, OUTPUT);
  pinMode(SENSOR2_XSHUT, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  digitalWrite(SENSOR1_XSHUT, LOW);
  digitalWrite(SENSOR2_XSHUT, LOW);
  delay(20);

  // Start with a visible heartbeat so users without serial can see progress
  setLedPattern(LED_HEARTBEAT);

  configureSensor(sensor1, SENSOR1_XSHUT, SENSOR1_ADDR);
  configureSensor(sensor2, SENSOR2_XSHUT, SENSOR2_ADDR);

  connectToWiFi();

  // Initialize DHT sensor
  dht.begin();

  // Initialize INA226 power monitor
  if (!ina226.init())
  {
    setLedPattern(LED_ERROR);
  }
  else
  {
    ina226.setResistorRange(SHUNT_RESISTOR_OHM, MAX_EXPECTED_CURRENT_A);
    ina226.setAverage(INA226_AVERAGE_16);
    ina226.setConversionTime(INA226_CONV_TIME_1100, INA226_CONV_TIME_1100);
    ina226.setMeasureMode(INA226_CONTINUOUS);
    setLedPattern(LED_SUCCESS, false, LED_HEARTBEAT);
  }

  // Perform NTP sync after WiFi connects so telemetry timestamps are accurate
  syncTimeWithNtp();

  connectToMQTT();
}

void loop()
{
  reconnectWiFi();

  if (!mqttClient.connected())
  {
    reconnectMQTT();
  }
  mqttClient.loop();

  printAndMaybeRecover(sensor1, SENSOR1_XSHUT, SENSOR1_ADDR, "Sensor 1: ", sensor1Failures);
  printAndMaybeRecover(sensor2, SENSOR2_XSHUT, SENSOR2_ADDR, "Sensor 2: ", sensor2Failures);

  publishTelemetry();

  uint32_t sleepMs = calculatePublishIntervalMs(currentFillPercentage);

  sleepWithLed(sleepMs);
}