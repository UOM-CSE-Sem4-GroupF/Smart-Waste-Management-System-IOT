#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>

constexpr uint8_t SENSOR1_XSHUT = 16;
constexpr uint8_t SENSOR2_XSHUT = 17;
constexpr uint8_t SENSOR1_ADDR = 0x30;
constexpr uint8_t SENSOR2_ADDR = 0x31;
constexpr uint8_t LED_PIN = 2;
constexpr int MAX_RECOVER_ATTEMPTS = 5;
constexpr int RECOVER_DELAY_MS = 200;
constexpr int MAX_CONSECUTIVE_FAILURES = 3;

VL53L0X sensor1;
VL53L0X sensor2;

int sensor1Failures = 0;
int sensor2Failures = 0;

void resetI2CBus()
{
  Wire.end();
  delay(10);
  Wire.begin();
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
  Wire.begin();

  pinMode(SENSOR1_XSHUT, OUTPUT);
  pinMode(SENSOR2_XSHUT, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  digitalWrite(SENSOR1_XSHUT, LOW);
  digitalWrite(SENSOR2_XSHUT, LOW);
  delay(20);

  configureSensor(sensor1, SENSOR1_XSHUT, SENSOR1_ADDR);
  configureSensor(sensor2, SENSOR2_XSHUT, SENSOR2_ADDR);

  Serial.println("Two ToF sensors initialized.");
}

void loop()
{
  printAndMaybeRecover(sensor1, SENSOR1_XSHUT, SENSOR1_ADDR, "Sensor 1: ", sensor1Failures);
  printAndMaybeRecover(sensor2, SENSOR2_XSHUT, SENSOR2_ADDR, "Sensor 2: ", sensor2Failures);

  Serial.println("---");
  delay(200);
}