// #define DEBUG

#include <stdarg.h>
#include <stdio.h>

#include <Adafruit_DPS310.h>
#include <Adafruit_LC709203F.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_ThinkInk.h>
#include <SensirionI2CScd4x.h>
#include <Wire.h>

static constexpr uint32_t VERSION_MAJOR = 0;
static constexpr uint32_t VERSION_MINOR = 1;
static constexpr uint32_t VERSION_PATCH = 0;

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
static constexpr int16_t ERROR_SIZE = 3;  // text size of error

static constexpr uint32_t DISPLAY_WAIT = 180; // wait between display updates in seconds

static constexpr uint32_t NUM_MEASUREMENTS = 2; // number of measurements to take
static constexpr uint32_t MEASUREMENT_WAIT = 5; // wait between checking measurement in seconds
static constexpr uint16_t CO2_LIMIT = 1000;     // CO2 ppm limit

static constexpr int16_t BATT_WIDTH = 3 * FOOTER_SIZE * CHAR_WIDTH;
static constexpr float BATT_LIMIT = 15.0f;

static constexpr size_t MESSAGE_SIZE = 256;

static ThinkInk_213_Tricolor_RW display(EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);

static SensirionI2CScd4x scd4x;

static Adafruit_LC709203F lc;

static Adafruit_DPS310 dps;

static uint32_t error;
static char message[MESSAGE_SIZE];

typedef enum
{
  ALIGN_LEFT,
  ALIGN_CENTER,
  ALIGN_RIGHT
} Alignment;

typedef enum
{
  ERROR_NONE,
  ERROR_CO2_SENSOR,
  ERROR_PRESSURE_SENSOR,
  ERROR_BATT_SENSOR,
} Error;

void checkSCD4xError(const uint16_t scd4xError)
{
  if (scd4xError)
  {
    error = (ERROR_CO2_SENSOR << 16) | scd4xError;
    errorToString(scd4xError, message, MESSAGE_SIZE);
  }
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
  Serial.print("Adanet CO2 Monitor v");
  Serial.print(VERSION_MAJOR);
  Serial.print(".");
  Serial.print(VERSION_MINOR);
  Serial.print(".");
  Serial.println(VERSION_PATCH);
#else
  // TODO(drw): is this necessary?
  delay(1000);
#endif

  // setup dps310 pressure sensor
  if (!dps.begin_I2C())
  {
    error = ERROR_PRESSURE_SENSOR << 16;
    snprintf(message, MESSAGE_SIZE, "Failed to find DPS");
  }

  if (error == ERROR_NONE)
  {
    dps.configureTemperature(DPS310_64HZ, DPS310_64SAMPLES);
    dps.configurePressure(DPS310_64HZ, DPS310_64SAMPLES);

    // setup scd4x co2 sensor
    Wire.begin();
    scd4x.begin(Wire);

    checkSCD4xError(scd4x.startPeriodicMeasurement());
  }

  uint32_t measurements = 0;
  uint16_t co2 = 0, pressure = 0;
  float temperature = 0.0f, humidity = 0.0f;
  while ((error == ERROR_NONE) && (measurements++ < NUM_MEASUREMENTS))
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
      Serial.println(temp_event.temperature);
      Serial.print(F("Pressure = "));
      Serial.println(pressure_event.pressure);
#endif

      pressure = static_cast<uint16_t>(pressure_event.pressure);
      checkSCD4xError(scd4x.setAmbientPressure(pressure));
    }
    else
    {
      error = ERROR_PRESSURE_SENSOR << 16;
      snprintf(message, MESSAGE_SIZE, "Pressure data not available");
    }

    if (error == ERROR_NONE)
    {
      // try to read measurement
      checkSCD4xError(scd4x.readMeasurement(co2, temperature, humidity));
    }

#ifdef DEBUG
    if (error == ERROR_NONE)
    {
      Serial.print("CO2 = ");
      Serial.println(co2);
      Serial.print("Temperature = ");
      Serial.println(temperature);
      Serial.print("Humidity = ");
      Serial.println(humidity);
    }
#endif
  }

  if (error == ERROR_NONE)
  {
    checkSCD4xError(scd4x.stopPeriodicMeasurement());
  }

  if ((error == ERROR_NONE) && (co2 == 0))
  {
    error = ERROR_CO2_SENSOR << 16;
    snprintf(message, MESSAGE_SIZE, "Invalid sample detected");
  }

  // setup lc709203f battery fuel gauge
  if ((error == ERROR_NONE) && !lc.begin())
  {
    error = ERROR_BATT_SENSOR << 16;
    snprintf(message, MESSAGE_SIZE, "Couldn't find LC709203F, make sure a battery is plugged in");
  }

  float batt = 0.0f;
  if (error == ERROR_NONE)
  {
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

    batt = lc.cellPercent();
    lc.setPowerMode(LC709203F_POWER_SLEEP);

#ifdef DEBUG
    Serial.print("Battery = ");
    Serial.println(batt);
#endif
  }

  // turn off i2c power
  digitalWrite(I2C_POWER, LOW);

#ifdef DEBUG
  if (error != ERROR_NONE)
  {
    Serial.print("Error = ");
    Serial.println(error, HEX);
    Serial.print("Message = ");
    Serial.println(message);
  }
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

  const int16_t headerY = 0;
  const int16_t bodyY = (display.height() - BODY_SIZE * CHAR_HEIGHT) / 2;
  const int16_t footerY = display.height() - 1 - FOOTER_SIZE * CHAR_HEIGHT;
  if (error == ERROR_NONE)
  {
    printfAligned(HEADER_SIZE, ALIGN_LEFT, headerY, EPD_BLACK, "% 3.1f%cC", temperature, 0xF7);
    printfAligned(HEADER_SIZE, ALIGN_RIGHT, headerY, EPD_BLACK, "%3.1f%%", humidity);

    const uint16_t co2Color = co2 >= CO2_LIMIT ? EPD_RED : EPD_BLACK;
    printfAligned(BODY_SIZE, ALIGN_CENTER, bodyY, co2Color, "%u", co2);

    const uint16_t battColor = batt < BATT_LIMIT ? EPD_RED : EPD_BLACK;
    const int16_t x0 = FOOTER_SIZE * CHAR_WIDTH / 2;
    const int16_t y0 = footerY;
    const int16_t w = BATT_WIDTH;
    const int16_t h = FOOTER_SIZE * CHAR_HEIGHT;
    const int16_t x1 = x0 + w;
    const int16_t y1 = y0 + h;
    display.fillRect(x0, y0, static_cast<int16_t>(batt / 100.0f * static_cast<float>(w)), h, battColor);
    display.drawRect(x0, y0, w, h, battColor);
    display.fillRect(x1, y0 + h / 4, FOOTER_SIZE * CHAR_WIDTH / 4, h / 2, battColor);
    printfAligned(FOOTER_SIZE, ALIGN_RIGHT, footerY, EPD_BLACK, "%u hPa", pressure);
  }
  else
  {
    display.setTextSize(ERROR_SIZE);
    display.setTextColor(EPD_BLACK);
    display.setCursor(0, 0);
    display.println(ESP.getEfuseMac(), HEX);
    display.print("v");
    display.println(VERSION_MAJOR);
    display.print(".");
    display.print(VERSION_MINOR);
    display.print(".");
    display.println(VERSION_PATCH);
    display.setTextColor(EPD_RED);
    display.println(error, HEX);
    display.println(message);
  }

  display.display(true);

  esp_sleep_enable_timer_wakeup((DISPLAY_WAIT - measurements * MEASUREMENT_WAIT) * 1000000ull);
  esp_deep_sleep_start();
#endif
}

void loop()
{
  // shouldn't reach here during normal operation
  delay(10);
}