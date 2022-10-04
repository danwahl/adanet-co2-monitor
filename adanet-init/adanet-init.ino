#include <stdarg.h>
#include <stdio.h>

#include <Adafruit_DPS310.h>
#include <Adafruit_LC709203F.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_ThinkInk.h>
#include <Wire.h>
#include <SensirionI2CScd4x.h>

static constexpr int16_t EPD_DC = 10;    // can be any pin, but required!
static constexpr int16_t EPD_CS = 9;     // can be any pin, but required!
static constexpr int16_t SRAM_CS = 6;    // can set to -1 to not use a pin (uses a lot of RAM!)
static constexpr int16_t EPD_RESET = -1; // can set to -1 and share with chip Reset (can't deep sleep)
static constexpr int16_t EPD_BUSY = -1;  // can set to -1 to not use a pin (will wait a fixed delay)

static constexpr int16_t CHAR_WIDTH = 6;  // width of a character in pixels
static constexpr int16_t CHAR_HEIGHT = 8; // height of a character in pixels

static constexpr uint32_t MEASUREMENT_WAIT = 5;   // wait between checking measurement in seconds
static constexpr uint32_t CALIBRATION_WAIT = 300; // wait between measurement start and calibration
static constexpr uint16_t CO2_AMBIENT = 415;      // Ambient CO2 for calibration

static ThinkInk_213_Tricolor_RW display(EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);

static SensirionI2CScd4x scd4x;

static Adafruit_LC709203F lc;

static Adafruit_DPS310 dps;

void setup()
{
  // turn on i2c power
  digitalWrite(I2C_POWER, HIGH);

  // turn off neopixel power
  digitalWrite(NEOPIXEL_POWER, !NEOPIXEL_POWER_ON);

  Serial.begin(115200);
  while (!Serial)
    delay(10);
  Serial.println("Adanet Init");

  // setup display
  display.begin(THINKINK_TRICOLOR);

  // enum | orientation | USB
  // -----|-------------|-------
  //   0  | landscape   | right
  //   1  | portrait    | top
  //   2  | landscape   | left
  //   3  | portrait    | bottom
  display.setRotation(2);

  display.clearBuffer();

  display.setTextSize(3);
  display.setTextColor(EPD_BLACK);
  display.setCursor(0, 0);

  display.println("Adanet Init");
  display.println("Calibrating");
  display.display(true);

  const uint32_t cpuFreq = getCpuFrequencyMhz();
  Serial.print("CPU Freq = ");
  Serial.print(cpuFreq);
  Serial.println(" MHz");
  const uint32_t xtalFreq = getXtalFrequencyMhz();
  Serial.print("XTAL Freq = ");
  Serial.print(xtalFreq);
  Serial.println(" MHz");
  const uint32_t apbFreq = getApbFrequency();
  Serial.print("APB Freq = ");
  Serial.print(apbFreq);
  Serial.println(" Hz");

  // setup dps310 pressure sensor
  if (!dps.begin_I2C())
  {
    Serial.println("Failed to find DPS");
    return;
  }

  dps.configureTemperature(DPS310_64HZ, DPS310_64SAMPLES);
  dps.configurePressure(DPS310_64HZ, DPS310_64SAMPLES);

  // setup scd4x co2 sensor
  Wire.begin();
  scd4x.begin(Wire);

  uint16_t serial0, serial1, serial2;
  if (scd4x.getSerialNumber(serial0, serial1, serial2) != 0)
  {
    Serial.println("Failed to read SCD4x serial number");
    return;
  }

  const uint64_t scd4xSerial =
      static_cast<uint64_t>(serial0) << 32 |
      static_cast<uint64_t>(serial1) << 16 |
      static_cast<uint64_t>(serial2);
  Serial.print("SCD4x serial number = ");
  Serial.println(scd4xSerial, HEX);

  if (scd4x.setAutomaticSelfCalibration(0) != 0)
  {
    Serial.println("Failed to disable SCD4x ASC");
    return;
  }

  if (scd4x.startPeriodicMeasurement() != 0)
  {
    Serial.println("Failed to start SCD4x measurement");
    return;
  }

  // pre-calibration measurement loop
  while (millis() < CALIBRATION_WAIT * 1000)
  {
    // sleep while waiting for next measurement
    delay(MEASUREMENT_WAIT * 1000);

    if (!dps.temperatureAvailable() || !dps.pressureAvailable())
    {
      Serial.println("Failed to read DPS");
      return;
    }

    sensors_event_t temp_event, pressure_event;
    dps.getEvents(&temp_event, &pressure_event);
    Serial.print(F("Temperature = "));
    Serial.println(temp_event.temperature);
    Serial.print(F("Pressure = "));
    Serial.println(pressure_event.pressure);

    if (scd4x.setAmbientPressure(pressure_event.pressure) != 0)
    {
      Serial.println("Failed to set SCD4x ambient pressure");
      return;
    }

    uint16_t co2 = 0;
    float temperature = 0.0f, humidity = 0.0f;

    if (scd4x.readMeasurement(co2, temperature, humidity) != 0)
    {
      Serial.println("Failed to read SCD4x measurement");
      return;
    }

    Serial.print("CO₂ = ");
    Serial.println(co2);
    Serial.print("Temperature = ");
    Serial.println(temperature);
    Serial.print("Humidity = ");
    Serial.println(humidity);

    Serial.println();
  }

  if (scd4x.stopPeriodicMeasurement() != 0)
  {
    Serial.println("Failed to stop SCD4x measurement");
    return;
  }

  uint16_t frcCorrection = 0;
  if ((scd4x.performForcedRecalibration(CO2_AMBIENT, frcCorrection) != 0) || (frcCorrection == 0xffff))
  {
    Serial.println("Failed to perform SCD4x forced recalibration");
    return;
  }

  Serial.print("Forced recalibration correction = ");
  Serial.println(static_cast<int16_t>(frcCorrection - 0x8000));

  if (scd4x.persistSettings() != 0)
  {
    Serial.println("Failed to persist SCD4x settings");
    return;
  }

  // setup lc709203f battery fuel gauge
  if (!lc.begin())
  {
    Serial.println("Couldn't find LC709203F, make sure a battery is plugged in");
    return;
  }

  Serial.println(F("Found LC709203F"));
  Serial.print("Version: 0x");
  Serial.println(lc.getICversion(), HEX);

  lc.setThermistorB(3950);
  Serial.print("Thermistor B = ");
  Serial.println(lc.getThermistorB());

  lc.setPackSize(LC709203F_APA_2000MAH);
  lc.setAlarmVoltage(3.8);

  const float batt = lc.cellPercent();
  lc.setPowerMode(LC709203F_POWER_SLEEP);

  Serial.print("Battery = ");
  Serial.println(batt);

  // turn off i2c power
  digitalWrite(I2C_POWER, LOW);

  display.println("Finished!");
  display.display(true);
}

void loop()
{
  delay(10);
}