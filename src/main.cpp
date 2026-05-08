#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_system.h>
#include <DHT.h>
// Try to include user config if present
#if defined(__has_include)
#if __has_include("config.h")
#include "config.h"
#endif
#endif

// ============ GPIO & Sensor Configuration ============
constexpr uint8_t SENSOR1_XSHUT = 16;
constexpr uint8_t SENSOR2_XSHUT = 17;
constexpr uint8_t SENSOR1_ADDR = 0x30;
constexpr uint8_t SENSOR2_ADDR = 0x31;
constexpr uint8_t LED_PIN = 2;
constexpr uint8_t BATTERY_ADC_PIN = 34; // ADC pin for battery voltage
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
constexpr const char *BIN_ID = "BIN-EDGE-001";
constexpr uint8_t ZONE_ID = 1;
constexpr const char *WASTE_CATEGORY = "general"; // food_waste, general, paper, plastic, glass
constexpr uint16_t BIN_VOLUME_LITRES = 240;
constexpr const char *FIRMWARE_VERSION = "2.1.4";

// ============ WiFi & MQTT Configuration ============
const char *WIFI_SSID = "ZTE Blade V50 Design";
const char *WIFI_PASSWORD = "102938asdf";
const char *MQTT_BROKER = "mqtt.example.com"; // TODO: Configure MQTT
constexpr uint16_t MQTT_PORT = 1883;
const char *MQTT_USER = "sensor-device";       // TODO: Configure MQTT user
const char *MQTT_PASSWORD = "swms-sensor-dev"; // TODO: Configure MQTT password
const char *MQTT_TOPIC_PREFIX = "sensors";

// ============ Sensor Objects ============
VL53L0X sensor1;
VL53L0X sensor2;

// ============ Sensor State ============
int sensor1Failures = 0;
int sensor2Failures = 0;

// ============ WiFi & MQTT Objects ============
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ============ Telemetry State ============
float currentFillPercentage = 0.0;
float batteryPercentage = 100.0;
int signalStrengthDbm = -70;
float temperatureCelsius = 25.0;

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
  Serial.print("Configuring NTP: ");
  Serial.println(NTP_SERVER_STR);
  long gmtOffsetSec = (long)(UTC_OFFSET_HOURS * 3600);
  configTime(gmtOffsetSec, 0, NTP_SERVER_STR);

  Serial.print("Waiting for NTP sync...");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 24 * 3600 && attempts < 10)
  {
    delay(1000);
    Serial.print('.');
    now = time(nullptr);
    attempts++;
  }
  Serial.println();
  if (now < 24 * 3600)
  {
    Serial.println("NTP sync failed or time not yet set");
  }
  else
  {
    Serial.println("NTP time synchronized");
    // Print the current ISO8601 UTC timestamp to the serial monitor
    String iso = getIsoUtcTimestamp();
    Serial.print("Current UTC time: ");
    Serial.println(iso);
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
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20)
  {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("\nFailed to connect to WiFi");
  }
}

void reconnectWiFi()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi disconnected. Reconnecting...");
    connectToWiFi();
  }
}

// ============ MQTT Functions ============
void onMqttConnect()
{
  Serial.print("MQTT connected to ");
  Serial.print(MQTT_BROKER);
  Serial.print(":");
  Serial.println(MQTT_PORT);
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("Message arrived on topic: ");
  Serial.println(topic);
}

void connectToMQTT()
{
  Serial.print("Connecting to MQTT broker: ");
  Serial.print(MQTT_BROKER);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

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
    Serial.println("MQTT connection failed");
  }
}

void reconnectMQTT()
{
  if (!mqttClient.connected())
  {
    Serial.println("MQTT disconnected. Reconnecting...");
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
  // For now: assume 0mm (full) = 0% and 240mm (empty) = 100%
  const uint16_t EMPTY_DISTANCE_MM = 240; // distance when bin is empty
  const uint16_t FULL_DISTANCE_MM = 0;    // distance when bin is full

  if (distanceMm >= EMPTY_DISTANCE_MM)
    return 0.0; // Empty
  if (distanceMm <= FULL_DISTANCE_MM)
    return 100.0; // Full

  float fillPct = 100.0 * (EMPTY_DISTANCE_MM - distanceMm) / (EMPTY_DISTANCE_MM - FULL_DISTANCE_MM);
  return constrain(fillPct, 0.0, 100.0);
}

float readBatteryPercentage()
{
  // TODO: Replace with actual battery voltage ADC reading
  // For now return a simulated value
  static float fakeBattery = 95.0;
  fakeBattery -= random(0, 5) * 0.001; // Slow drain
  if (fakeBattery < 15.0)
    fakeBattery = 95.0; // Simulate replacement
  return constrain(fakeBattery, 0.0, 100.0);
}

float readTemperature()
{
  // Use DHT sensor to read temperature (Celsius)
  float t = dht.readTemperature();
  if (isnan(t))
  {
    // preserve previous value if read failed
    Serial.println("DHT read failed, keeping previous temperature");
    return temperatureCelsius;
  }
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

  if (fillPercentage < 50.0)
    return 600000; // 10 minutes
  else if (fillPercentage < 75.0)
    return 300000; // 5 minutes
  else if (fillPercentage < 90.0)
    return 120000; // 2 minutes
  else
    return 30000; // 30 seconds (urgent)
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
  doc["signal_strength_dbm"] = signalStrengthDbm;
  doc["temperature_c"] = round(temperatureCelsius * 10.0) / 10.0;

  // TODO: Replace with actual NTP timestamp
  doc["timestamp"] = "2026-05-08T00:00:00Z";

  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["error_flags"] = 0; // TODO: Add error detection logic

  // Serialize and publish
  String payload;
  serializeJson(doc, payload);

  String topic = String(MQTT_TOPIC_PREFIX) + "/bin/" + BIN_ID + "/telemetry";

  if (mqttClient.publish(topic.c_str(), payload.c_str(), true))
  {
    Serial.print("Published: ");
    Serial.print(topic);
    Serial.print(" = ");
    Serial.println(payload);
  }
  else
  {
    Serial.println("Failed to publish telemetry");
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
    Serial.println("Failed to initialize ToF sensor");
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
  for (int attempt = 1; attempt <= MAX_RECOVER_ATTEMPTS; ++attempt)
  {
    Serial.print("Recovering sensor on XSHUT ");
    Serial.print(xshutPin);
    Serial.print(" attempt ");
    Serial.println(attempt);

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
      Serial.println("init() failed during recovery");
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
      Serial.println("Recovery successful");
      digitalWrite(LED_PIN, LOW);
      return true;
    }

    Serial.println("Still timing out after reinit");
    digitalWrite(LED_PIN, LOW);
    delay(RECOVER_DELAY_MS);
  }

  Serial.println("Recovery attempts exhausted");
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
    Serial.print(label);
    Serial.print(distance);
    Serial.println(" mm");
    return;
  }

  ++failureCount;
  Serial.print(label);
  Serial.print("fault (read=");
  Serial.print(distance);
  Serial.print(", failures=");
  Serial.print(failureCount);
  Serial.println(")");

  if (failureCount < MAX_CONSECUTIVE_FAILURES)
  {
    return;
  }

  Serial.println("Attempting sensor recovery");
  if (recoverSensor(sensor, xshutPin, address))
  {
    failureCount = 0;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== SWMS Edge Device Starting ===\n");
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Reset reason: ");
  Serial.print(static_cast<int>(reason));
  Serial.print(" (");
  Serial.print(resetReasonToString(reason));
  Serial.println(")");

  Wire.begin();

  pinMode(SENSOR1_XSHUT, OUTPUT);
  pinMode(SENSOR2_XSHUT, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BATTERY_ADC_PIN, INPUT);

  digitalWrite(SENSOR1_XSHUT, LOW);
  digitalWrite(SENSOR2_XSHUT, LOW);
  delay(20);

  configureSensor(sensor1, SENSOR1_XSHUT, SENSOR1_ADDR);
  configureSensor(sensor2, SENSOR2_XSHUT, SENSOR2_ADDR);

  Serial.println("Two ToF sensors initialized.");

  connectToWiFi();

  // Initialize DHT sensor
  dht.begin();

  // Perform NTP sync after WiFi connects so telemetry timestamps are accurate
  syncTimeWithNtp();

  connectToMQTT();

  Serial.println("Setup complete!\n");
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

  Serial.print("Fill: ");
  Serial.print(currentFillPercentage, 1);
  Serial.print("% | Battery: ");
  Serial.print(batteryPercentage, 1);
  Serial.print("% | Next publish in ");
  Serial.print(sleepMs / 1000);
  Serial.println("s");
  Serial.println("---");

  delay(sleepMs);
}