#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include "sleep_kitty_bitmap.h"
#include <time.h>
#include <math.h>
// #include "driver/gpio.h"

// ============================================================
// MINI OLEG — single-screen weather clock
// Build: MINI-019-CLOCK-BOTTOM-ALIGN — internal battery bolt + lower-edge clock alignment
// Board: ESP32-C3 Super Mini (Tenstar Robot)
// Display: Waveshare 1.51" Transparent OLED, SSD1309, 128x64
// Interface: factory 4-wire SPI
//
// Wiring:
//   OLED VCC -> 3V3
//   OLED GND -> GND
//   OLED DIN -> GPIO10
//   OLED CLK -> GPIO8
//   OLED CS  -> GPIO7
//   OLED DC  -> GPIO6
//   OLED RST -> GPIO5
//
// Battery ADC on GPIO1:
//   BAT+ -> 100 kOhm -> GPIO1 -> 100 kOhm -> GND
//   Battery/HU-009 GND -> ESP32-C3 GND
//
// The old audio-presence detector is retired. GPIO1 is battery-only.
//
// Configuration:
//   First boot automatically starts the local setup portal.
//   AP:       MINI-OLEG-SETUP
//   Password: olegsetup
//   URL:      http://192.168.4.1
//
//   To reopen the portal later, hold the BOOT button for 5 seconds
//   AFTER the main UI has already appeared. Do not hold BOOT during
//   power-on/reset because GPIO9 is the ESP32-C3 boot strap pin.
// ============================================================

// ---------------- Display pins ----------------

// Hardware-passed direct JST wiring. OLED VCC must be 3V3, not 5V.
static constexpr uint8_t OLED_DIN = 10;
static constexpr uint8_t OLED_CLK = 8;
static constexpr uint8_t OLED_CS  = 7;
static constexpr uint8_t OLED_DC  = 6;
static constexpr uint8_t OLED_RST = 5;

// Initial hardware-passed Dupont pinout kept as diagnostic history.
// static constexpr uint8_t OLED_DIN = 6;
// static constexpr uint8_t OLED_CLK = 4;
// static constexpr uint8_t OLED_CS  = 7;
// static constexpr uint8_t OLED_DC  = 3;
// static constexpr uint8_t OLED_RST = 10;

// On-board BOOT button on the common ESP32-C3 Super Mini layout.
static constexpr uint8_t BOOT_BUTTON_PIN = 9;
static constexpr uint8_t BATTERY_ADC_PIN = 1;  // ADC1_CH1


// U8g2 software 4-wire SPI. Pins are explicitly defined for the hardware-passed JST layout.
U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI u8g2(
  U8G2_R0,
  OLED_CLK,
  OLED_DIN,
  OLED_CS,
  OLED_DC,
  OLED_RST
);

// ---------------- Runtime constants ----------------
static constexpr char BUILD_VERSION[] = "MINI-019-CLOCK-BOTTOM-ALIGN";
static constexpr char AP_SSID[] = "MINI-OLEG-SETUP";
static constexpr char AP_PASSWORD[] = "olegsetup";
static constexpr char DEVICE_HOSTNAME[] = "mini-oleg";

// Ukraine: EET UTC+2, EEST UTC+3, current last-Sunday DST rule.
static constexpr char TZ_INFO[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";

static constexpr uint32_t WEATHER_UPDATE_INTERVAL_MS = 60UL * 60UL * 1000UL;
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30UL * 1000UL;
static constexpr uint32_t NTP_RETRY_INTERVAL_MS = 60UL * 1000UL;
static constexpr uint32_t WEATHER_RETRY_INTERVAL_MS = 60UL * 1000UL;
static constexpr uint32_t UI_REDRAW_INTERVAL_MS = 500UL;
static constexpr uint32_t BOOT_HOLD_FOR_PORTAL_MS = 5000UL;
static constexpr uint32_t PORTAL_REBOOT_DELAY_MS = 1800UL;
static constexpr uint32_t BATTERY_SAMPLE_INTERVAL_MS = 1000UL;
static constexpr uint32_t BATTERY_DEBUG_INTERVAL_MS = 5000UL;
static constexpr uint8_t BATTERY_SAMPLE_COUNT = 16;
static constexpr uint16_t BATTERY_SAMPLE_GAP_US = 250;

// GPIO1 sees half of BAT+ through the agreed 100k/100k divider.
static constexpr float BATTERY_DIVIDER_RATIO = 2.0f;

// Physical calibration knobs. Keep 1.0 / 0.0 for the first hardware pass,
// then tune against a multimeter without touching the ADC/UI code.
static constexpr float BATTERY_VOLTAGE_SCALE = 1.000f;
static constexpr float BATTERY_VOLTAGE_OFFSET = 0.000f;

// Provisional Li-ion endpoints for the first visual test.
// The 8-pixel fill is intentionally coarse; final values will be calibrated
// on the assembled HU-009 battery path.
static constexpr float BATTERY_EMPTY_VOLTAGE = 3.30f;
static constexpr float BATTERY_FULL_VOLTAGE = 4.20f;
static constexpr float BATTERY_VALID_MIN_VOLTAGE = 2.50f;
static constexpr float BATTERY_VALID_MAX_VOLTAGE = 4.50f;

// Status-column geometry copied from the real 128x64 screen:
// Wi-Fi (9 px wide) -> vertical battery of the same width.
// The lightning glyph is XOR-drawn inside the battery, so it automatically
// turns black over filled pixels and white over empty pixels.
static constexpr int STATUS_ICON_X = 1;
static constexpr int STATUS_ICON_WIDTH = 9;
static constexpr int WIFI_ICON_Y = 33;
static constexpr int BATTERY_TERMINAL_Y = 43;

static constexpr uint32_t SLEEP_Z_ANIMATION_MS = 650UL;

// ---------------- Stored configuration ----------------
struct DeviceConfig {
  String wifiSsid;
  String wifiPass;
  String weatherApiKey;
  String weatherLocation;
  bool valid = false;
};

DeviceConfig deviceConfig;
WebServer portalServer(80);
DNSServer dnsServer;

bool portalActive = false;
bool portalHandlersRegistered = false;
uint32_t rebootAtMs = 0;

// ---------------- Weather / time state ----------------
enum class WeatherKind : uint8_t {
  UNKNOWN,
  CLEAR,
  CLOUD,
  RAIN,
  SNOW,
  STORM,
  FOG
};

float weatherTempC = NAN;
int weatherHumidity = -1;
int weatherConditionId = -1;
bool weatherIsNight = false;
bool weatherValid = false;
WeatherKind weatherKind = WeatherKind::UNKNOWN;

bool ntpConfigured = false;
bool ntpValid = false;

uint32_t lastWeatherAttemptMs = 0;
uint32_t lastNtpAttemptMs = 0;
uint32_t lastWiFiAttemptMs = 0;
uint32_t lastUiDrawMs = 0;
uint32_t bootButtonPressedAtMs = 0;

uint32_t lastBatterySampleMs = 0;
uint32_t lastBatteryDebugMs = 0;
uint16_t batteryAdcMilliVolts = 0;
float batteryVoltage = NAN;
uint8_t batteryPercent = 0;
bool batteryValid = false;
bool forceUiRedraw = true;

// ============================================================
// Small helpers
// ============================================================

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  return value;
}

String urlEncode(const String& input) {
  static const char hex[] = "0123456789ABCDEF";
  String output;
  output.reserve(input.length() * 3);

  for (size_t i = 0; i < input.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(input[i]);
    const bool unreserved =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~' || c == ',';

    if (unreserved) {
      output += static_cast<char>(c);
    } else {
      output += '%';
      output += hex[(c >> 4) & 0x0F];
      output += hex[c & 0x0F];
    }
  }

  return output;
}

bool timeIsValid() {
  const time_t now = time(nullptr);
  return now >= 1704067200; // 2024-01-01 UTC
}

String twoDigits(int value) {
  char buffer[4];
  snprintf(buffer, sizeof(buffer), "%02d", value);
  return String(buffer);
}

const char* ukrainianWeekdayUpper(int tmWday) {
  static const char* const days[] = {
    "НЕДІЛЯ", "ПОНЕДІЛОК", "ВІВТОРОК", "СЕРЕДА",
    "ЧЕТВЕР", "П'ЯТНИЦЯ", "СУБОТА"
  };

  if (tmWday < 0 || tmWday > 6) return "--";
  return days[tmWday];
}

const char* ukrainianMonthGenitive(int tmMon) {
  static const char* const months[] = {
    "січня", "лютого", "березня", "квітня",
    "травня", "червня", "липня", "серпня",
    "вересня", "жовтня", "листопада", "грудня"
  };

  if (tmMon < 0 || tmMon > 11) return "--";
  return months[tmMon];
}

WeatherKind classifyWeather(int conditionId) {
  if (conditionId >= 200 && conditionId < 300) return WeatherKind::STORM;
  if (conditionId >= 300 && conditionId < 600) return WeatherKind::RAIN;
  if (conditionId >= 600 && conditionId < 700) return WeatherKind::SNOW;
  if (conditionId >= 700 && conditionId < 800) return WeatherKind::FOG;
  if (conditionId == 800) return WeatherKind::CLEAR;
  if (conditionId > 800 && conditionId < 900) return WeatherKind::CLOUD;
  return WeatherKind::UNKNOWN;
}

const char* weatherStatusUpper() {
  if (!weatherValid) return "--";

  switch (weatherKind) {
    case WeatherKind::CLEAR: return weatherIsNight ? "ЯСНО" : "СОНЦЕ";
    case WeatherKind::CLOUD: return "ХМАРНО";
    case WeatherKind::RAIN:  return "ДОЩ";
    case WeatherKind::SNOW:  return "СНІГ";
    case WeatherKind::STORM: return "ГРОЗА";
    case WeatherKind::FOG:   return "ТУМАН";
    case WeatherKind::UNKNOWN:
    default:                 return "--";
  }
}

// ============================================================
// Preferences / NVS
// ============================================================

void loadConfig() {
  deviceConfig = DeviceConfig{};
  deviceConfig.weatherLocation = "Kyiv,UA";

  Preferences prefs;
  if (!prefs.begin("mini_oleg", true)) {
    Serial.println("Config: cannot open NVS");
    return;
  }

  deviceConfig.valid = prefs.getBool("valid", false);
  deviceConfig.wifiSsid = prefs.getString("ssid", "");
  deviceConfig.wifiPass = prefs.getString("pass", "");
  deviceConfig.weatherApiKey = prefs.getString("api", "");
  deviceConfig.weatherLocation = prefs.getString("loc", "Kyiv,UA");
  prefs.end();

  deviceConfig.wifiSsid.trim();
  deviceConfig.weatherApiKey.trim();
  deviceConfig.weatherLocation.trim();

  deviceConfig.valid =
    deviceConfig.valid &&
    deviceConfig.wifiSsid.length() != 0 &&
    deviceConfig.weatherApiKey.length() != 0 &&
    deviceConfig.weatherLocation.length() != 0;

  Serial.println(deviceConfig.valid ? "Config: loaded" : "Config: setup required");
}

bool saveConfig() {
  deviceConfig.wifiSsid.trim();
  deviceConfig.weatherApiKey.trim();
  deviceConfig.weatherLocation.trim();

  if (deviceConfig.wifiSsid.length() == 0 ||
      deviceConfig.weatherApiKey.length() == 0 ||
      deviceConfig.weatherLocation.length() == 0) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin("mini_oleg", false)) return false;

  prefs.putBool("valid", true);
  prefs.putString("ssid", deviceConfig.wifiSsid);
  prefs.putString("pass", deviceConfig.wifiPass);
  prefs.putString("api", deviceConfig.weatherApiKey);
  prefs.putString("loc", deviceConfig.weatherLocation);
  prefs.end();

  deviceConfig.valid = true;
  return true;
}

void loadCachedWeather() {
  Preferences prefs;
  if (!prefs.begin("mini_weather", true)) return;

  const bool cachedValid = prefs.getBool("valid", false);
  if (cachedValid) {
    weatherTempC = prefs.getFloat("temp", NAN);
    weatherHumidity = prefs.getInt("hum", -1);
    weatherConditionId = prefs.getInt("cond", -1);
    weatherIsNight = prefs.getBool("night", false);

    weatherValid =
      !isnan(weatherTempC) &&
      weatherHumidity >= 0 && weatherHumidity <= 100 &&
      weatherConditionId > 0;

    weatherKind = classifyWeather(weatherConditionId);
  }

  prefs.end();
}

void saveCachedWeather() {
  if (!weatherValid) return;

  Preferences prefs;
  if (!prefs.begin("mini_weather", false)) return;

  prefs.putBool("valid", true);
  prefs.putFloat("temp", weatherTempC);
  prefs.putInt("hum", weatherHumidity);
  prefs.putInt("cond", weatherConditionId);
  prefs.putBool("night", weatherIsNight);
  prefs.end();
}

// ============================================================
// GPIO1 battery ADC
// ============================================================

uint8_t batteryVoltageToPercent(float voltage) {
  if (!isfinite(voltage)) return 0;
  if (voltage <= BATTERY_EMPTY_VOLTAGE) return 0;
  if (voltage >= BATTERY_FULL_VOLTAGE) return 100;

  const float normalized =
    (voltage - BATTERY_EMPTY_VOLTAGE) /
    (BATTERY_FULL_VOLTAGE - BATTERY_EMPTY_VOLTAGE);

  int percent = static_cast<int>(lroundf(normalized * 100.0f));
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return static_cast<uint8_t>(percent);
}

uint16_t readBatteryAdcMilliVolts() {
  // The 100k/100k divider is deliberately low-current. A few throwaway reads
  // let the ADC sample capacitor settle before the averaged measurement.
  analogRead(BATTERY_ADC_PIN);
  delayMicroseconds(BATTERY_SAMPLE_GAP_US);
  analogRead(BATTERY_ADC_PIN);
  delayMicroseconds(BATTERY_SAMPLE_GAP_US);

  uint32_t sumMilliVolts = 0;
  for (uint8_t i = 0; i < BATTERY_SAMPLE_COUNT; ++i) {
    sumMilliVolts += static_cast<uint32_t>(analogReadMilliVolts(BATTERY_ADC_PIN));
    delayMicroseconds(BATTERY_SAMPLE_GAP_US);
  }

  return static_cast<uint16_t>(sumMilliVolts / BATTERY_SAMPLE_COUNT);
}

void serviceBattery(bool force = false) {
  const uint32_t now = millis();
  if (!force && now - lastBatterySampleMs < BATTERY_SAMPLE_INTERVAL_MS) return;
  lastBatterySampleMs = now;

  const uint16_t measuredMilliVolts = readBatteryAdcMilliVolts();
  const float measuredVoltage =
    (static_cast<float>(measuredMilliVolts) / 1000.0f) *
    BATTERY_DIVIDER_RATIO *
    BATTERY_VOLTAGE_SCALE +
    BATTERY_VOLTAGE_OFFSET;

  batteryAdcMilliVolts = measuredMilliVolts;

  const bool measurementValid =
    isfinite(measuredVoltage) &&
    measuredVoltage >= BATTERY_VALID_MIN_VOLTAGE &&
    measuredVoltage <= BATTERY_VALID_MAX_VOLTAGE;

  if (measurementValid) {
    // First sample locks immediately; later readings use a light EMA to keep
    // the tiny fill bar steady while Wi-Fi and the OLED are active.
    if (!batteryValid || !isfinite(batteryVoltage)) {
      batteryVoltage = measuredVoltage;
    } else {
      batteryVoltage = batteryVoltage * 0.75f + measuredVoltage * 0.25f;
    }

    batteryPercent = batteryVoltageToPercent(batteryVoltage);
    batteryValid = true;
  } else {
    batteryVoltage = measuredVoltage;
    batteryPercent = 0;
    batteryValid = false;
  }

  if (force || now - lastBatteryDebugMs >= BATTERY_DEBUG_INTERVAL_MS) {
    lastBatteryDebugMs = now;
    Serial.print("Battery ADC: ");
    Serial.print(batteryAdcMilliVolts);
    Serial.print(" mV / BAT: ");
    Serial.print(batteryVoltage, 3);
    Serial.print(" V / ");
    if (batteryValid) {
      Serial.print(batteryPercent);
      Serial.println("%");
    } else {
      Serial.println("invalid");
    }
  }
}

// ============================================================
// OLED UI primitives
// ============================================================

void drawWiFiIcon(int x, int y, bool connected) {
  // Compact 9x10 OLEG-style Wi-Fi glyph.
  u8g2.drawPixel(x + 0, y + 5);
  u8g2.drawPixel(x + 1, y + 4);
  u8g2.drawPixel(x + 2, y + 3);
  u8g2.drawPixel(x + 3, y + 3);
  u8g2.drawPixel(x + 4, y + 2);
  u8g2.drawPixel(x + 5, y + 3);
  u8g2.drawPixel(x + 6, y + 3);
  u8g2.drawPixel(x + 7, y + 4);
  u8g2.drawPixel(x + 8, y + 5);

  u8g2.drawPixel(x + 2, y + 6);
  u8g2.drawPixel(x + 3, y + 5);
  u8g2.drawPixel(x + 4, y + 4);
  u8g2.drawPixel(x + 5, y + 5);
  u8g2.drawPixel(x + 6, y + 6);

  u8g2.drawPixel(x + 3, y + 7);
  u8g2.drawPixel(x + 4, y + 6);
  u8g2.drawPixel(x + 5, y + 7);
  u8g2.drawPixel(x + 4, y + 9);

  if (!connected) {
    u8g2.drawLine(x + 8, y + 1, x + 1, y + 9);
    u8g2.drawLine(x + 8, y + 2, x + 2, y + 9);
  }
}

void drawBatteryIcon(int x, int terminalY, uint8_t percent, bool valid) {
  // Overall width is exactly 9 px, matching drawWiFiIcon().
  // With the separate lower bolt removed, the body now reaches y=62,
  // the same lower edge as the month name on the real OLED.
  static constexpr int BODY_Y = 2;
  static constexpr int BODY_W = STATUS_ICON_WIDTH;
  static constexpr int BODY_H = 18;
  static constexpr int INNER_X = 1;
  static constexpr int INNER_Y = 1;
  static constexpr int INNER_W = 7;
  static constexpr int INNER_H = 16;

  // Positive terminal.
  u8g2.drawBox(x + 3, terminalY, 3, 2);

  // Vertical battery body: y=45..62.
  const int bodyY = terminalY + BODY_Y;
  u8g2.drawFrame(x, bodyY, BODY_W, BODY_H);

  if (valid && percent > 0) {
    uint8_t fillRows = static_cast<uint8_t>(
      (static_cast<uint16_t>(percent) * INNER_H + 50U) / 100U
    );
    if (fillRows == 0) fillRows = 1;
    if (fillRows > INNER_H) fillRows = INNER_H;

    const int fillY = bodyY + INNER_Y + INNER_H - fillRows;
    u8g2.drawBox(x + INNER_X, fillY, INNER_W, fillRows);
  }

  // 7x8 lightning centered inside the battery. XOR drawing gives the
  // requested automatic inversion: white over empty area, black over fill.
  static const char* const boltRows[] = {
    "....##.",
    "...##..",
    "..##...",
    ".#####.",
    "...##..",
    "..##...",
    ".##....",
    "##....."
  };

  const int boltX = x + INNER_X;
  const int boltY = bodyY + 5;
  u8g2.setDrawColor(2);
  for (uint8_t row = 0; row < 8; ++row) {
    for (uint8_t col = 0; col < INNER_W; ++col) {
      if (boltRows[row][col] == '#') {
        u8g2.drawPixel(boltX + col, boltY + row);
      }
    }
  }
  u8g2.setDrawColor(1);
}

void drawSunIcon(int x, int y) {
  u8g2.drawDisc(x + 10, y + 8, 5);
  u8g2.drawLine(x + 10, y + 0, x + 10, y + 2);
  u8g2.drawLine(x + 10, y + 14, x + 10, y + 17);
  u8g2.drawLine(x + 1, y + 8, x + 3, y + 8);
  u8g2.drawLine(x + 17, y + 8, x + 19, y + 8);
  u8g2.drawLine(x + 3, y + 1, x + 5, y + 3);
  u8g2.drawLine(x + 15, y + 3, x + 17, y + 1);
  u8g2.drawLine(x + 3, y + 15, x + 5, y + 13);
  u8g2.drawLine(x + 15, y + 13, x + 17, y + 15);
}

void drawMoonIcon(int x, int y) {
  u8g2.drawDisc(x + 9, y + 8, 7);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(x + 13, y + 5, 7);
  u8g2.setDrawColor(1);
  u8g2.drawPixel(x + 18, y + 12);
  u8g2.drawPixel(x + 17, y + 13);
  u8g2.drawPixel(x + 18, y + 14);
}

void drawCloudIcon(int x, int y) {
  u8g2.drawDisc(x + 6, y + 10, 4);
  u8g2.drawDisc(x + 12, y + 7, 6);
  u8g2.drawDisc(x + 19, y + 10, 4);
  u8g2.drawBox(x + 4, y + 9, 18, 6);
}

void drawPartlyCloudyIcon(int x, int y) {
  drawSunIcon(x + 4, y);
  drawCloudIcon(x, y + 4);
}

void drawMoonCloudIcon(int x, int y) {
  drawMoonIcon(x + 5, y);
  drawCloudIcon(x, y + 4);
}

void drawRainIcon(int x, int y) {
  drawCloudIcon(x, y);
  u8g2.drawLine(x + 6, y + 16, x + 4, y + 19);
  u8g2.drawLine(x + 12, y + 16, x + 10, y + 19);
  u8g2.drawLine(x + 18, y + 16, x + 16, y + 19);
}

void drawSnowIcon(int x, int y) {
  drawCloudIcon(x, y);
  u8g2.drawPixel(x + 5, y + 18);
  u8g2.drawPixel(x + 11, y + 20);
  u8g2.drawPixel(x + 17, y + 18);
}

void drawStormIcon(int x, int y) {
  drawCloudIcon(x, y);
  u8g2.drawLine(x + 13, y + 15, x + 9, y + 19);
  u8g2.drawLine(x + 9, y + 19, x + 13, y + 19);
  u8g2.drawLine(x + 13, y + 19, x + 10, y + 22);
}

void drawFogIcon(int x, int y) {
  u8g2.drawHLine(x + 1, y + 5, 22);
  u8g2.drawHLine(x + 4, y + 10, 19);
  u8g2.drawHLine(x + 1, y + 15, 22);
}

void drawWeatherIcon(int x, int y) {
  if (!weatherValid) {
    u8g2.setFont(u8g2_font_6x13_tf);
    u8g2.drawStr(x + 7, y + 17, "?");
    return;
  }

  switch (weatherKind) {
    case WeatherKind::CLEAR:
      if (weatherIsNight) drawMoonIcon(x + 3, y + 1);
      else drawSunIcon(x + 2, y + 1);
      break;
    case WeatherKind::CLOUD:
      if (weatherIsNight) drawMoonCloudIcon(x, y);
      else drawPartlyCloudyIcon(x, y);
      break;
    case WeatherKind::RAIN:
      drawRainIcon(x, y + 1);
      break;
    case WeatherKind::SNOW:
      drawSnowIcon(x, y + 1);
      break;
    case WeatherKind::STORM:
      drawStormIcon(x, y);
      break;
    case WeatherKind::FOG:
      drawFogIcon(x, y + 2);
      break;
    case WeatherKind::UNKNOWN:
    default:
      u8g2.setFont(u8g2_font_6x13_tf);
      u8g2.drawStr(x + 7, y + 17, "?");
      break;
  }
}

void drawCenteredUtf8InBox(const String& text, int left, int width, int baselineY, const uint8_t* font) {
  u8g2.setFont(font);
  const int textWidth = u8g2.getUTF8Width(text.c_str());
  int x = left + (width - textWidth) / 2;
  if (x < left) x = left;
  u8g2.setCursor(x, baselineY);
  u8g2.print(text);
}

void drawHumidityBlock() {
  const String humidityText = weatherValid ? String(weatherHumidity) : "--";

  // Same numeric font and baseline as the temperature block.
  u8g2.setFont(u8g2_font_logisoso18_tn);
  const int numberWidth = u8g2.getUTF8Width(humidityText.c_str());

  u8g2.setFont(u8g2_font_6x10_tf);
  const int percentWidth = u8g2.getUTF8Width("%");

  const int totalWidth = numberWidth + 1 + percentWidth;
  int x = (42 - totalWidth) / 2;
  if (x < 1) x = 1;

  u8g2.setFont(u8g2_font_logisoso18_tn);
  u8g2.setCursor(x, 20);
  u8g2.print(humidityText);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(x + numberWidth + 1, 19);
  u8g2.print("%");

  drawCenteredUtf8InBox("ВОЛОГІСТЬ", 0, 42, 29, u8g2_font_4x6_t_cyrillic);
}

void drawWeatherBlock() {
  // Center top panel: icon above, uppercase state below.
  drawWeatherIcon(49, 0);
  drawCenteredUtf8InBox(
    String(weatherStatusUpper()),
    43,
    36,
    29,
    u8g2_font_4x6_t_cyrillic
  );
}

void drawTemperatureBlock(const struct tm* now) {
  String tempNumber = weatherValid ? String(static_cast<int>(lroundf(weatherTempC))) : "--";

  u8g2.setFont(u8g2_font_logisoso18_tn);
  const int numberWidth = u8g2.getUTF8Width(tempNumber.c_str());

  // Degree dot + C take about 9 px.
  const int totalWidth = numberWidth + 10;
  int x = 80 + (48 - totalWidth) / 2;
  if (x < 81) x = 81;

  u8g2.setFont(u8g2_font_logisoso18_tn);
  u8g2.setCursor(x, 20);
  u8g2.print(tempNumber);

  const int suffixX = x + numberWidth + 1;
  u8g2.drawCircle(suffixX + 2, 9, 2);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(suffixX + 5, 19, "C");

  const String weekday = now ? String(ukrainianWeekdayUpper(now->tm_wday)) : "--";
  drawCenteredUtf8InBox(weekday, 80, 48, 29, u8g2_font_4x6_t_cyrillic);
}

static constexpr int DATE_DIGIT_TOP_Y = 35;
static constexpr int LOWER_CONTENT_EDGE_Y = 62;

void drawClockBlock(const struct tm* now) {
  String timeText = "--:--";
  if (now) {
    timeText = twoDigits(now->tm_hour) + ":" + twoDigits(now->tm_min);
  }

  // Compact left status column. Width stays inside x=1..9.
  // The clock is shifted 2 px right for a calmer gap after the battery.
  drawWiFiIcon(STATUS_ICON_X, WIFI_ICON_Y, WiFi.status() == WL_CONNECTED);
  drawBatteryIcon(
    STATUS_ICON_X,
    BATTERY_TERMINAL_Y,
    batteryPercent,
    batteryValid
  );

  u8g2.setFont(u8g2_font_logisoso24_tn);
  const int textWidth = u8g2.getUTF8Width(timeText.c_str());

  // Align the visible lower edge of the large clock digits with the
  // lower edge of the month name. This intentionally replaces the old
  // top-edge alignment, which looked visually uneven on the real OLED.
  u8g2.setFontPosBottom();

  // Reserve the first 12 px for the complete status column and spacing.
  int x = 13;
  if (x + textWidth > 90) x = 90 - textWidth;
  if (x < 1) x = 1;

  u8g2.setCursor(x, LOWER_CONTENT_EDGE_Y);
  u8g2.print(timeText);
  u8g2.setFontPosBaseline();
}

void drawDateBlock(const struct tm* now) {
  const String dayText = now ? String(now->tm_mday) : "--";
  const String monthText = now ? String(ukrainianMonthGenitive(now->tm_mon)) : "--";

  u8g2.setFont(u8g2_font_logisoso18_tn);
  const int dayBaselineY = DATE_DIGIT_TOP_Y + u8g2.getAscent();

  // Start 3 px to the right of the divider. The date keeps its
  // established geometry; only the clock was lowered for bottom alignment.
  drawCenteredUtf8InBox(dayText, 94, 34, dayBaselineY, u8g2_font_logisoso18_tn);
  drawCenteredUtf8InBox(monthText, 94, 34, 62, u8g2_font_4x6_t_cyrillic);
}

void drawSleepZPixelGlyph(int x, int y, uint8_t scale) {
  const uint8_t width = scale * 3;
  u8g2.drawBox(x, y, width, scale);
  u8g2.drawBox(x, y + scale * 4, width, scale);

  for (uint8_t i = 0; i < 3; ++i) {
    u8g2.drawBox(x + scale * (2 - i), y + scale * (1 + i), scale, scale);
  }
}

void drawSleepZAnimation() {
  const uint8_t frame = (millis() / SLEEP_Z_ANIMATION_MS) % 3;

  drawSleepZPixelGlyph(57, 17, 1);
  if (frame >= 1) drawSleepZPixelGlyph(66, 12, 1);
  if (frame >= 2) drawSleepZPixelGlyph(76, 5, 2);
}

// Dormant easter egg for future generations.
// GPIO1 is battery-only in MINI-017; the kitty drawing code stays here
// intentionally and is not tied to battery or audio state.
void drawSleepScreen() {
  u8g2.clearBuffer();
  u8g2.drawXBMP(0, 0, KITTY_SLEEP_WIDTH, KITTY_SLEEP_HEIGHT, kitty_sleep_128x64);
  drawSleepZAnimation();
  u8g2.sendBuffer();
}

void drawMainScreen() {
  struct tm now;
  struct tm* nowPtr = nullptr;
  if (getLocalTime(&now, 20) && now.tm_year >= (2024 - 1900)) {
    nowPtr = &now;
    ntpValid = true;
  }

  u8g2.clearBuffer();

  // Main grid adapted from the approved mockup to the real 128x64 panel.
  u8g2.drawHLine(0, 31, 128);
  u8g2.drawVLine(42, 0, 32);
  u8g2.drawVLine(79, 0, 32);
  u8g2.drawVLine(91, 32, 32);

  drawHumidityBlock();
  drawWeatherBlock();
  drawTemperatureBlock(nowPtr);
  drawClockBlock(nowPtr);
  drawDateBlock(nowPtr);

  u8g2.sendBuffer();
}

void drawPortalScreen(const String& statusLine = "") {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawFrame(2, 2, 124, 60);

  drawCenteredUtf8InBox("НАЛАШТУВАННЯ", 0, 128, 14, u8g2_font_6x12_t_cyrillic);

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(8, 27, AP_SSID);
  u8g2.setFont(u8g2_font_4x6_t_cyrillic);
  u8g2.setCursor(8, 38);
  u8g2.print("ПАРОЛЬ: olegsetup");
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(18, 50, "192.168.4.1");

  if (statusLine.length() != 0) {
    drawCenteredUtf8InBox(statusLine, 0, 128, 60, u8g2_font_4x6_t_cyrillic);
  }

  u8g2.sendBuffer();
}

// ============================================================
// Configuration portal
// ============================================================

String buildPortalPage(const String& notice = "", bool error = false) {
  String html;
  html.reserve(5200);

  html += F("<!doctype html><html lang='uk'><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Mini OLEG Setup</title><style>");
  html += F("body{margin:0;background:#090d0f;color:#d9fbff;font-family:Arial,sans-serif}");
  html += F("main{max-width:520px;margin:auto;padding:18px}.card{background:#10191d;border:1px solid #1f5963;border-radius:14px;padding:16px;box-shadow:0 0 24px #00d9ff18}");
  html += F("h1{margin:4px 0 6px;color:#87f2ff}.hint{color:#9fb4b9;font-size:13px;line-height:1.45}");
  html += F("label{display:block;margin:14px 0 4px;color:#c7edf2;font-size:14px}input{box-sizing:border-box;width:100%;padding:11px;margin-top:5px;border-radius:9px;border:1px solid #31535a;background:#071013;color:#fff;font-size:16px}");
  html += F("button{width:100%;margin-top:18px;padding:12px;border:0;border-radius:10px;background:#75ecff;color:#041014;font-weight:700;font-size:16px}");
  html += F(".notice{padding:10px;border-radius:9px;background:#123b2a;border:1px solid #2e8059}.err{background:#421a1a;border-color:#9d3e3e}.small{font-size:12px;color:#78949a}");
  html += F("</style></head><body><main><h1>Mini OLEG</h1>");
  html += F("<p class='hint'>Локальне налаштування Wi-Fi та OpenWeather. Дані зберігаються всередині ESP32-C3.</p>");

  if (notice.length() != 0) {
    html += "<p class='notice";
    if (error) html += " err";
    html += "'>";
    html += htmlEscape(notice);
    html += "</p>";
  }

  html += F("<form class='card' method='POST' action='/save'>");

  html += F("<label>Wi-Fi SSID<input name='ssid' required value='");
  html += htmlEscape(deviceConfig.wifiSsid);
  html += F("'></label>");

  html += F("<label>Пароль Wi-Fi<input type='password' name='pass' value='");
  html += htmlEscape(deviceConfig.wifiPass);
  html += F("'></label>");

  html += F("<label>OpenWeather API key<input type='password' name='api' required value='");
  html += htmlEscape(deviceConfig.weatherApiKey);
  html += F("'></label>");

  html += F("<label>Локація<input name='loc' required value='");
  html += htmlEscape(deviceConfig.weatherLocation);
  html += F("'><span class='small'>Формат: Kyiv,UA або Lviv,UA. На екрані місто не показується.</span></label>");

  html += F("<button type='submit'>Зберегти й перезапустити</button></form>");
  html += F("<p class='hint'>AP: <b>MINI-OLEG-SETUP</b><br>Пароль: <b>olegsetup</b><br>Адреса: <b>192.168.4.1</b></p>");
  html += F("</main></body></html>");

  return html;
}

void portalHandleRoot() {
  portalServer.send(200, "text/html; charset=utf-8", buildPortalPage());
}

void portalHandleSave() {
  DeviceConfig newConfig = deviceConfig;
  newConfig.wifiSsid = portalServer.arg("ssid");
  newConfig.wifiPass = portalServer.arg("pass");
  newConfig.weatherApiKey = portalServer.arg("api");
  newConfig.weatherLocation = portalServer.arg("loc");

  newConfig.wifiSsid.trim();
  newConfig.weatherApiKey.trim();
  newConfig.weatherLocation.trim();

  if (newConfig.wifiSsid.length() == 0 ||
      newConfig.weatherApiKey.length() == 0 ||
      newConfig.weatherLocation.length() == 0) {
    portalServer.send(400, "text/html; charset=utf-8", buildPortalPage("Заповни SSID, API key і локацію.", true));
    return;
  }

  deviceConfig = newConfig;
  if (!saveConfig()) {
    portalServer.send(500, "text/html; charset=utf-8", buildPortalPage("Не вдалося записати конфігурацію в NVS.", true));
    return;
  }

  portalServer.send(200, "text/html; charset=utf-8", buildPortalPage("Збережено. Mini OLEG зараз перезапуститься."));
  drawPortalScreen("ЗБЕРЕЖЕНО");
  rebootAtMs = millis() + PORTAL_REBOOT_DELAY_MS;
}

void portalRedirectToRoot() {
  portalServer.sendHeader("Location", "http://192.168.4.1/", true);
  portalServer.send(302, "text/plain", "");
}

void registerPortalHandlersOnce() {
  if (portalHandlersRegistered) return;

  portalServer.on("/", HTTP_GET, portalHandleRoot);
  portalServer.on("/save", HTTP_POST, portalHandleSave);

  // Common captive-portal probes.
  portalServer.on("/generate_204", HTTP_GET, portalRedirectToRoot);
  portalServer.on("/hotspot-detect.html", HTTP_GET, portalRedirectToRoot);
  portalServer.on("/connecttest.txt", HTTP_GET, portalRedirectToRoot);
  portalServer.onNotFound(portalRedirectToRoot);

  portalHandlersRegistered = true;
}

bool startConfigPortal(const char* reason) {
  if (portalActive) return true;

  Serial.print("Portal start: ");
  Serial.println(reason);

  WiFi.disconnect(true, true);
  delay(150);
  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("Portal: softAP failed");
    drawPortalScreen("ПОМИЛКА AP");
    return false;
  }

  registerPortalHandlersOnce();
  dnsServer.start(53, "*", WiFi.softAPIP());
  portalServer.begin();
  portalActive = true;

  Serial.print("Portal IP: ");
  Serial.println(WiFi.softAPIP());
  drawPortalScreen("ВІДКРИЙ БРАУЗЕР");
  return true;
}

void serviceConfigPortal() {
  dnsServer.processNextRequest();
  portalServer.handleClient();

  if (rebootAtMs != 0 && static_cast<int32_t>(millis() - rebootAtMs) >= 0) {
    ESP.restart();
  }
}

// ============================================================
// Wi-Fi, NTP and weather
// ============================================================

bool connectToConfiguredWiFi(uint32_t timeoutMs = 15000UL) {
  if (!deviceConfig.valid) return false;
  if (WiFi.status() == WL_CONNECTED) return true;

  lastWiFiAttemptMs = millis();

  WiFi.mode(WIFI_MODE_NULL);
  delay(80);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(deviceConfig.wifiSsid.c_str(), deviceConfig.wifiPass.c_str());

  Serial.print("WiFi: connecting to ");
  Serial.println(deviceConfig.wifiSsid);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi: connected, IP ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("WiFi: connection failed");
  return false;
}

bool configureAndSyncTime() {
  if (WiFi.status() != WL_CONNECTED) return false;

  lastNtpAttemptMs = millis();

  if (!ntpConfigured) {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
    ntpConfigured = true;
  }

  const uint32_t startedAt = millis();
  while (millis() - startedAt < 12000UL) {
    if (timeIsValid()) {
      ntpValid = true;
      Serial.println("NTP: synced");
      return true;
    }
    delay(150);
  }

  ntpValid = false;
  Serial.println("NTP: sync failed");
  return false;
}

bool fetchWeather() {
  lastWeatherAttemptMs = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Weather: skipped, WiFi offline");
    return false;
  }

  if (deviceConfig.weatherApiKey.length() == 0 || deviceConfig.weatherLocation.length() == 0) {
    Serial.println("Weather: missing API key or location");
    return false;
  }

  const String url =
    "https://api.openweathermap.org/data/2.5/weather?q=" +
    urlEncode(deviceConfig.weatherLocation) +
    "&appid=" + deviceConfig.weatherApiKey +
    "&units=metric&lang=uk";

  WiFiClientSecure secureClient;
  // HTTPS transport is encrypted; certificate validation is deliberately
  // disabled in MINI-001 to avoid a baked-in CA expiry breaking the clock.
  secureClient.setInsecure();

  HTTPClient http;
  http.setTimeout(12000);

  if (!http.begin(secureClient, url)) {
    Serial.println("Weather: HTTP begin failed");
    return false;
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("Weather: HTTP ");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("Weather JSON: ");
    Serial.println(error.c_str());
    return false;
  }

  const float newTemp = doc["main"]["temp"] | NAN;
  const int newHumidity = doc["main"]["humidity"] | -1;
  const int newConditionId = doc["weather"][0]["id"] | -1;
  const char* iconCode = doc["weather"][0]["icon"] | "";

  if (isnan(newTemp) ||
      newHumidity < 0 || newHumidity > 100 ||
      newConditionId <= 0) {
    Serial.println("Weather: incomplete response");
    return false;
  }

  weatherTempC = newTemp;
  weatherHumidity = newHumidity;
  weatherConditionId = newConditionId;
  weatherIsNight = String(iconCode).endsWith("n");
  weatherKind = classifyWeather(weatherConditionId);
  weatherValid = true;
  saveCachedWeather();

  Serial.print("Weather: ");
  Serial.print(weatherTempC, 1);
  Serial.print(" C, ");
  Serial.print(weatherHumidity);
  Serial.println("%");
  return true;
}

void serviceNetwork() {
  if (!deviceConfig.valid || portalActive) return;

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
      connectToConfiguredWiFi(8000UL);
    }
    return;
  }

  if (!timeIsValid()) {
    if (lastNtpAttemptMs == 0 || millis() - lastNtpAttemptMs >= NTP_RETRY_INTERVAL_MS) {
      configureAndSyncTime();
    }
  } else {
    ntpValid = true;
  }

  const uint32_t weatherInterval = weatherValid
    ? WEATHER_UPDATE_INTERVAL_MS
    : WEATHER_RETRY_INTERVAL_MS;

  if (lastWeatherAttemptMs == 0 || millis() - lastWeatherAttemptMs >= weatherInterval) {
    fetchWeather();
  }
}

void serviceBootButton() {
  if (portalActive) return;

  const bool pressed = digitalRead(BOOT_BUTTON_PIN) == LOW;
  if (!pressed) {
    bootButtonPressedAtMs = 0;
    return;
  }

  if (bootButtonPressedAtMs == 0) {
    bootButtonPressedAtMs = millis();
    return;
  }

  if (millis() - bootButtonPressedAtMs >= BOOT_HOLD_FOR_PORTAL_MS) {
    bootButtonPressedAtMs = 0;
    startConfigPortal("BOOT runtime hold");
  }
}

// ============================================================
// Arduino entry points
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(120);
  Serial.println();
  Serial.print("Mini OLEG ");
  Serial.println(BUILD_VERSION);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);

  // Failed GPIO20/21 experiment kept as diagnostic history.
  // gpio_reset_pin(GPIO_NUM_20);
  // gpio_reset_pin(GPIO_NUM_21);
  // u8g2.setBusClock(100000);

  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(255);

  loadConfig();
  loadCachedWeather();
  serviceBattery(true);
  drawMainScreen();

  if (!deviceConfig.valid) {
    startConfigPortal("first boot / missing config");
    return;
  }

  if (connectToConfiguredWiFi()) {
    configureAndSyncTime();
    fetchWeather();
  }

  forceUiRedraw = false;
  drawMainScreen();
}

void loop() {
  if (portalActive) {
    serviceConfigPortal();
    delay(2);
    return;
  }

  serviceBootButton();
  if (portalActive) {
    delay(2);
    return;
  }

  serviceBattery();
  serviceNetwork();

  const uint32_t now = millis();
  if (forceUiRedraw || now - lastUiDrawMs >= UI_REDRAW_INTERVAL_MS) {
    forceUiRedraw = false;
    lastUiDrawMs = now;
    drawMainScreen();
  }

  delay(2);
}