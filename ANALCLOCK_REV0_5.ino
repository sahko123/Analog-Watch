#include <Wire.h>
#include <MCP4726_wire1.h>
#include <math.h>
#include "time.h"
#include "sys/time.h"

MCP4726 dac;

int current_timezone_offset = 1; // hours offset from UTC

// Cubic shape: v = A*x + B*x^2 + C*x^3,  A+B+C=1 (exact endpoints)
// Least-squares fit to measured points: 0%=0, 25%=555, 50%=960, 75%=1555, 100%=2650
#define LUT_A  1.107f
#define LUT_B -1.416f
#define LUT_C  1.309f   // = 1 - A - B
#define LUT_SIZE 13

uint16_t meter_lut[LUT_SIZE];
const int lut_size = LUT_SIZE;

void buildLUT(uint16_t maxValue) {
  for (int i = 0; i < LUT_SIZE; i++) {
    float x = (float)i / (LUT_SIZE - 1);
    float v = LUT_A * x + LUT_B * x * x + LUT_C * x * x * x;
    meter_lut[i] = (uint16_t)(maxValue * v);
  }
}

unsigned long lastUpdate = 0;
bool usePowerFit = false;

// Buttons: active high, external pull-down, high-impedance input
#define BTN_LOW 23
#define BTN_MID 24
#define BTN_TOP 25

enum DisplayMode { MODE_SECONDS, MODE_HOURS, MODE_DATE, MODE_MONTH, MODE_DOW, MODE_STATIC_DAC };
DisplayMode displayMode = MODE_SECONDS;
uint16_t    staticDacValue = 0;

const unsigned long DEBOUNCE_MS     = 20;  // switch bounce < 10ms; 20ms is safe
const unsigned long DOUBLE_CLICK_MS = 1500;

// Rising-edge debounce: accepts a press only if it arrives >= DEBOUNCE_MS after
// the previous accepted press. Release state is ignored, which avoids the failure
// mode where a quick re-press arrives before the release debounce clears.
struct Button {
  uint8_t       pin;
  bool          lastRaw;
  unsigned long lastEdgeTime;

  void begin(uint8_t p) {
    pin          = p;
    lastRaw      = false;
    lastEdgeTime = 0;
  }

  // Returns true once per physical press.
  bool risingEdge(unsigned long now) {
    bool raw  = digitalRead(pin);
    bool edge = false;
    if (raw && !lastRaw && now - lastEdgeTime >= DEBOUNCE_MS) {
      lastEdgeTime = now;
      edge         = true;
    }
    lastRaw = raw;
    return edge;
  }
};

Button btnTop, btnMid, btnLow;

// Double-click tracking
static unsigned long topLastPress  = 0;
static int           topPressCount = 0;
static unsigned long midLastPress  = 0;
static int           midPressCount = 0;

uint16_t getInterpolatedDAC(float percent);
uint16_t getPowerFitDAC(float percent);

void setup() {
  Serial.begin(115200);
  Wire1.setSDA(2);
  Wire1.setSCL(3);
  Wire1.begin();
  dac.begin(0x61);
  buildLUT(2650);

  pinMode(BTN_LOW, INPUT);
  pinMode(BTN_MID, INPUT);
  pinMode(BTN_TOP, INPUT);

  btnTop.begin(BTN_TOP);
  btnMid.begin(BTN_MID);
  btnLow.begin(BTN_LOW);

  dac.setVoltage(0);

  struct timeval tv;
  tv.tv_sec = 1611198855; // Jan 21, 2021  3:14:15AM — RPi Pico release
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

void loop() {
  // Serial handler
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input == "bootloader") {
      rp2040.rebootToBootloader();

    } else if (input == "reset") {
      rp2040.reboot();

    } else if (input == "lut") {
      // Print current LUT values
      Serial.print("meter_lut[] = {");
      for (int i = 0; i < LUT_SIZE; i++) {
        Serial.print(meter_lut[i]);
        if (i < LUT_SIZE - 1) Serial.print(", ");
      }
      Serial.println("};");

    } else if (input.startsWith("tz ")) {
      current_timezone_offset = input.substring(3).toInt();
      Serial.print("Timezone offset = ");
      Serial.println(current_timezone_offset);

    } else if (input.startsWith("dac ")) {
      // Hold needle at a fixed value for calibration: "dac 2048"
      staticDacValue = (uint16_t)input.substring(4).toInt();
      displayMode    = MODE_STATIC_DAC;
      Serial.print("Static DAC = ");
      Serial.println(staticDacValue);

    } else if (input.startsWith("max ")) {
      // Set full-scale max and rebuild LUT via cubic: "max 3500"
      uint16_t newMax = (uint16_t)input.substring(4).toInt();
      buildLUT(newMax);
      Serial.print("max=");
      Serial.print(newMax);
      Serial.println(", LUT rebuilt");

    } else if (input.startsWith("set ")) {
      // Set a single LUT entry: "set 6 1100"
      int space = input.indexOf(' ', 4);
      if (space > 0) {
        int   idx = input.substring(4, space).toInt();
        uint16_t val = (uint16_t)input.substring(space + 1).toInt();
        if (idx >= 0 && idx < LUT_SIZE) {
          meter_lut[idx] = val;
          Serial.print("lut[");
          Serial.print(idx);
          Serial.print("] = ");
          Serial.println(val);
        } else {
          Serial.println("index out of range");
        }
      }

    } else {
      // Treat as Unix timestamp
      time_t newTime = (time_t)input.toInt() + (current_timezone_offset * 3600);
      struct timeval tv = { .tv_sec = newTime, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
      Serial.print("Time set to: ");
      Serial.println(newTime);
    }
  }

  unsigned long now = millis();

  // Top button: single press = hours, double click within 1.5s = date
  if (btnTop.risingEdge(now)) {
    if (topPressCount > 0 && now - topLastPress < DOUBLE_CLICK_MS) {
      displayMode   = MODE_DATE;
      topPressCount = 0;
      Serial.println("Mode: DATE");
    } else {
      topPressCount = 1;
      topLastPress  = now;
      displayMode   = MODE_HOURS;
      Serial.println("Mode: HOURS");
    }
  }

  // Mid button: single press = month, double click within 1.5s = seconds
  if (btnMid.risingEdge(now)) {
    if (midPressCount > 0 && now - midLastPress < DOUBLE_CLICK_MS) {
      displayMode   = MODE_SECONDS;
      midPressCount = 0;
      Serial.println("Mode: SECONDS");
    } else {
      midPressCount = 1;
      midLastPress  = now;
      displayMode   = MODE_MONTH;
      Serial.println("Mode: MONTH");
    }
  }

  // Low button: press = day of week
  if (btnLow.risingEdge(now)) {
    displayMode = MODE_DOW;
    Serial.println("Mode: DAY_OF_WEEK");
  }

  // 200ms display update
  if (now - lastUpdate >= 200) {
    lastUpdate += 200;

    // Static calibration mode: hold needle at a fixed DAC value
    if (displayMode == MODE_STATIC_DAC) {
      dac.setVoltage(staticDacValue);
      Serial.print("STATIC DAC = ");
      Serial.println(staticDacValue);
      return;
    }

    time_t epochNow;
    time(&epochNow);
    struct tm* t = localtime(&epochNow);

    float percent;

    switch (displayMode) {

      case MODE_HOURS: {
        int h = t->tm_hour % 12;
        if (h == 0) {
          percent = t->tm_min / 60.0;  // 12 o'clock hour: sweep minutes across full scale
        } else {
          percent = (h + t->tm_min / 60.0) / 12.0;
        }
        Serial.print("HOURS ");
        Serial.print(h == 0 ? 12 : h);
        Serial.print(":");
        if (t->tm_min < 10) Serial.print("0");
        Serial.print(t->tm_min);
        break;
      }

      case MODE_DATE: {
        percent = t->tm_mday / 60.0;  // day 1–31 on the 0–60 seconds scale
        Serial.print("DATE ");
        Serial.print(t->tm_mday);
        break;
      }

      case MODE_MONTH: {
        // Dec→Nov order: Dec(tm_mon=11)=idx 0, Jan(0)=idx 1 … Nov(10)=idx 11
        int idx = (t->tm_mon + 1) % 12;
        percent = (idx + 0.5) / 12.0;
        const char* labels[] = {"D","J","F","M","A","M","J","J","A","S","O","N"};
        Serial.print("MONTH ");
        Serial.print(labels[idx]);
        break;
      }

      case MODE_DOW: {
        // Mon→Sun order: tm_wday Mon=1…Sat=6,Sun=0 → idx 0–6
        int idx = (t->tm_wday + 6) % 7;
        percent = (idx + 0.5) / 7.0;
        const char* labels[] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
        Serial.print("DOW ");
        Serial.print(labels[idx]);
        break;
      }

      case MODE_SECONDS:
      default: {
        percent = t->tm_sec / 60.0;
        Serial.print("SEC ");
        Serial.print(t->tm_sec);
        break;
      }
    }

    uint16_t dacValue = usePowerFit ? getPowerFitDAC(percent) : getInterpolatedDAC(percent);
    dac.setVoltage(dacValue);
    Serial.print(" → DAC = ");
    Serial.println(dacValue);
  }
}

// Interpolation between LUT values (with gamma smoothing)
uint16_t getInterpolatedDAC(float percent) {
  percent = constrain(percent, 0.0, 1.0);
  float position = percent * (lut_size - 1);
  int   index    = (int)position;
  float t        = position - index;
  t = pow(t, 1.5);
  if (index >= lut_size - 1) return meter_lut[lut_size - 1];
  uint16_t start = meter_lut[index];
  uint16_t end   = meter_lut[index + 1];
  return (uint16_t)(start + (end - start) * t);
}

// Power-law fitted model (from your full calibration curve)
uint16_t getPowerFitDAC(float percent) {
  percent = constrain(percent, 0.0, 1.0);
  return (uint16_t)(2940.06 * pow(percent, 1.40));
}
