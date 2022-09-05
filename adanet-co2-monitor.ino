// #define DEBUG

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

static constexpr int16_t HEADER_SIZE = 3; // text size of header
static constexpr int16_t BODY_SIZE = 6;   // text size of body
static constexpr int16_t FOOTER_SIZE = 3; // text size of footer

static constexpr uint32_t DISPLAY_WAIT = 180; // wait between display updates in seconds

static constexpr uint16_t NUM_MEASUREMENTS = 2; // number of measurements to take
static constexpr uint32_t MEASUREMENT_WAIT = 5; // wait between checking measurement in seconds
static constexpr uint16_t CO2_LIMIT = 1000;     // CO2 ppm limit
// static constexpr uint16_t SENSOR_ALTITUDE = 181; // altitude in meters

static constexpr int16_t BATT_WIDTH = 3 * FOOTER_SIZE * CHAR_WIDTH;
static constexpr float BAT_LIMIT = 15.0f;

static ThinkInk_213_Tricolor_RW display(EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);

static SensirionI2CScd4x scd4x;

static Adafruit_LC709203F lc;

Adafruit_NeoPixel neoPixel = Adafruit_NeoPixel(NEOPIXEL_NUM, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

static Adafruit_DPS310 dps;

typedef enum
{
  ALIGN_LEFT,
  ALIGN_CENTER,
  ALIGN_RIGHT
} Alignment;

void printSCD4xError(uint16_t error, const char *msg)
{
#ifdef DEBUG
  if (error)
  {
    char errorMessage[256];
    Serial.print(msg);
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
  }
#endif
}

void printfAligned(const uint8_t size, const Alignment alignment, const int16_t y, const uint16_t color, const char *fmt, ...)
{
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  display.setTextSize(size);
  display.setTextColor(color);
  switch (alignment)
  {
  case ALIGN_CENTER:
    display.setCursor((display.width() - strlen(buffer) * size * CHAR_WIDTH) / 2, y);
    break;
  case ALIGN_RIGHT:
    display.setCursor(display.width() - 1 - strlen(buffer) * size * CHAR_WIDTH, y);
    break;
  // ALIGN_LEFT
  default:
    display.setCursor(0, y);
    break;
  }
  display.print(buffer);
}

void setup()
{
  // turn on i2c power
  digitalWrite(I2C_POWER, HIGH);

  // turn off neopixel power
  digitalWrite(NEOPIXEL_POWER, !NEOPIXEL_POWER_ON);

#ifdef DEBUG
  Serial.begin(115200);
  while (!Serial)
    delay(10);
  Serial.println("Adanet - CO2 monitor");
#else
  delay(1000);
#endif

// setup dps310 pressure sensor
#ifdef DEBUG
  Serial.println("DPS310");
#endif
  if (!dps.begin_I2C())
  {
#ifdef DEBUG
    Serial.println("Failed to find DPS");
#endif
    while (1)
      yield();
  }
#ifdef DEBUG
  Serial.println("DPS OK!");
#endif

  dps.configurePressure(DPS310_64HZ, DPS310_64SAMPLES);
  dps.configureTemperature(DPS310_64HZ, DPS310_64SAMPLES);

  // setup scd4x co2 sensor
  uint16_t error;
  Wire.begin();
  scd4x.begin(Wire);

  error = scd4x.setAutomaticSelfCalibration(0);
  printSCD4xError(error, "Error trying to execute setAutomaticSelfCalibration(): ");

  // TODO(drw): can be removed once pressure sensor is verified
  // error = scd4x.setSensorAltitude(SENSOR_ALTITUDE);
  // printSCD4xError(error, "Error trying to execute setSensorAltitude(): ");

  error = scd4x.startPeriodicMeasurement();
  printSCD4xError(error, "Error trying to execute startPeriodicMeasurement(): ");

  uint16_t co2 = 0, pressure = 0;
  float temperature = 0.0f, humidity = 0.0f;
  for (uint16_t i = 0; i < NUM_MEASUREMENTS; i++)
  {
    // sleep while waiting for next measurement
#ifdef DEBUG
    delay(MEASUREMENT_WAIT * 1000);
#else
    esp_sleep_enable_timer_wakeup(MEASUREMENT_WAIT * 1000000ull);
    esp_light_sleep_start();
#endif

    if (dps.temperatureAvailable() && dps.pressureAvailable())
    {
      sensors_event_t temp_event, pressure_event;

      dps.getEvents(&temp_event, &pressure_event);
#ifdef DEBUG
      Serial.print(F("Temperature = "));
      Serial.print(temp_event.temperature);
      Serial.println(" *C");

      Serial.print(F("Pressure = "));
      Serial.print(pressure_event.pressure);
      Serial.println(" hPa");
#endif

      pressure = static_cast<uint16_t>(pressure_event.pressure);
      error = scd4x.setAmbientPressure(pressure);
      printSCD4xError(error, "Error trying to execute setAmbientPressure(): ");
    }

    // try to read measurement
    error = scd4x.readMeasurement(co2, temperature, humidity);
    printSCD4xError(error, "Error trying to execute readMeasurement(): ");

    if (co2 == 0)
    {
#ifdef DEBUG
      Serial.println("Invalid sample detected, skipping.");
#endif
    }
    else
    {
#ifdef DEBUG
      Serial.print("CO2:");
      Serial.print(co2);
      Serial.print("\t");
      Serial.print("Temperature:");
      Serial.print(temperature);
      Serial.print("\t");
      Serial.print("Humidity:");
      Serial.println(humidity);
#endif
    }
  }

  error = scd4x.stopPeriodicMeasurement();
  printSCD4xError(error, "Error trying to execute stopPeriodicMeasurement(): ");

  // setup lc709203f battery fuel gauge
  if (!lc.begin())
  {
    // TODO(drw): proper error handling here and elsewhere
#ifdef DEBUG
    Serial.println(F("Couldn't find Adafruit LC709203F?\nMake sure a battery is plugged in!"));
    while (1)
      delay(10);
#endif
  }
#ifdef DEBUG
  Serial.println(F("Found LC709203F"));
  Serial.print("Version: 0x");
  Serial.println(lc.getICversion(), HEX);
#endif

  lc.setThermistorB(3950);
#ifdef DEBUG
  Serial.print("Thermistor B = ");
  Serial.println(lc.getThermistorB());
#endif

  lc.setPackSize(LC709203F_APA_2000MAH);
  lc.setAlarmVoltage(3.8);

  const float batt = lc.cellPercent();
  lc.setPowerMode(LC709203F_POWER_SLEEP);

#ifdef DEBUG
  Serial.print("Battery:");
  Serial.println(batt);
#endif

  // turn off i2c power
  digitalWrite(I2C_POWER, LOW);

#ifdef DEBUG
  Serial.println();
#else
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

  printfAligned(HEADER_SIZE, ALIGN_LEFT, 0, EPD_BLACK, "% 3.1f%cC", temperature, 0xF7);
  printfAligned(HEADER_SIZE, ALIGN_RIGHT, 0, EPD_BLACK, "%3.1f%%", humidity);

  const int16_t bodyY = (display.height() - BODY_SIZE * CHAR_HEIGHT) / 2;
  const uint16_t co2Color = co2 >= CO2_LIMIT ? EPD_RED : EPD_BLACK;
  printfAligned(BODY_SIZE, ALIGN_CENTER, bodyY, co2Color, "%u", co2);

  const int16_t footerY = display.height() - 1 - FOOTER_SIZE * CHAR_HEIGHT;
  const uint16_t battColor = batt < 15.0f ? EPD_RED : EPD_BLACK;
  const int16_t x0 = FOOTER_SIZE * CHAR_WIDTH / 2;
  const int16_t y0 = footerY;
  const int16_t w = BATT_WIDTH;
  const int16_t h = FOOTER_SIZE * CHAR_HEIGHT;
  const int16_t x1 = x0 + w;
  const int16_t y1 = y0 + h;
  display.fillRect(x0, y0, static_cast<int16_t>(batt / 100.0f * static_cast<float>(w)), h, battColor);
  display.drawRect(x0, y0, w, h, battColor);
  display.fillRect(x1, y0 + h / 4, FOOTER_SIZE * CHAR_WIDTH / 4, h / 2, battColor);
  // printfAligned(FOOTER_SIZE, ALIGN_RIGHT, footerY, EPD_BLACK, "CO%c ppm", 0xFC);
  printfAligned(HEADER_SIZE, ALIGN_RIGHT, footerY, EPD_BLACK, "%u hPa", pressure);

  display.display(true);
  // TODO(drw): pull EPD_RESET low, once connected
  // delay(1500);

  esp_sleep_enable_timer_wakeup((DISPLAY_WAIT - NUM_MEASUREMENTS * MEASUREMENT_WAIT) * 1000000ull);
  esp_deep_sleep_start();
#endif
}

void loop()
{
  // shouldn't reach here during normal operation
  delay(10);
}