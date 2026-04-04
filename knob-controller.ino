#include "connectwifilogic.h"
#include "autoupdatelogic.h"
#include "webserver.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <WebServer.h>
#include "globals.h"
#include "oled_disply.h"
WebServer server(80); 

// Order matters: Include rotary before blelogic so 'display' is available
#include "rotarycode.h"
#include "blelogic.h"

#define BUZZER_PIN 5

// --- NTP Configuration ---
// IST (India/Pune) = UTC+5:30 = 19800 seconds
#define NTP_SERVER      "pool.ntp.org"
#define UTC_OFFSET_SEC  19800
#define DST_OFFSET_SEC  0


// --- Standby Tracking ---
unsigned long lastActivityTime  = 0;
unsigned long lastStandbyUpdate = 0;
int lastStandbyCounter          = 0;



unsigned long lastDisplayUpdate = 0;
unsigned long lastBeepTime      = 0;
bool beepState = false;

// --- Hourly Chime Tracking ---
int lastChimeHour = -1;

// --- NTP cache ---
bool      ntpEverSynced  = false;
struct tm cachedTimeinfo = {};
unsigned long cachedTimeMillis = 0;

// =============================================================
// TIME HELPER  (cache-aware, never flickers back to "No Time")
// =============================================================
bool getTime(struct tm &timeinfo) {
  if (getLocalTime(&timeinfo, 1000)) {
    cachedTimeinfo   = timeinfo;
    cachedTimeMillis = millis();
    ntpEverSynced    = true;
    return true;
  }
  if (ntpEverSynced) {
    unsigned long elapsedSec = (millis() - cachedTimeMillis) / 1000UL;
    time_t cachedEpoch = mktime(&cachedTimeinfo);
    cachedEpoch += elapsedSec;
    localtime_r(&cachedEpoch, &timeinfo);
    cachedTimeinfo   = timeinfo;
    cachedTimeMillis = millis();
    return true;
  }
  return false;
}

// =============================================================
// BUZZER HELPERS
// =============================================================

// Short tactile click — played on every confirmed button press
// Very short so it doesn't delay anything
void playClickTone() {
  tone(BUZZER_PIN, 1800, 18); // high-pitched, 18 ms — snappy and satisfying
  // No delay needed: tone() is non-blocking, the 18ms duration
  // expires on its own while the loop continues running
}

// Long-press confirmation — slightly lower and longer
void playLongPressTone() {
  tone(BUZZER_PIN, 900, 60);
}

void playHourlyChime() {
  tone(BUZZER_PIN, 1047, 120); // C6
  delay(160);
  tone(BUZZER_PIN, 1319, 200); // E6
  delay(240);
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
}

// =============================================================
// ICON DRAWING
// =============================================================

const unsigned char epd_bitmap_wifi_icon [] PROGMEM = {
  0x03, 0x80, 0x0f, 0xf0, 0x3f, 0xfc, 0xfc, 0x3f, 0x61, 0x8e, 0x0f, 0xf0,
  0x1f, 0xf8, 0x0e, 0x70, 0x00, 0x00, 0x03, 0xc0, 0x01, 0x80, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

void drawWifiIcon(int cx, int cy, bool connected) {
  if (connected) {
    display.drawBitmap(cx, cy, epd_bitmap_wifi_icon, 16, 16, SSD1306_WHITE);
  }
  if (!connected) {
    display.drawLine(cx + 2, cy + 12, cx + 10, cy, SSD1306_WHITE);
  }
}

void drawBTIcon(int x, int y, bool paired) {
  int cx     = x + 5;
  int top    = y;
  int bot    = y + 12;
  int lx     = x + 1;
  int rx     = x + 9;
  int upperY = y + 3;
  int lowerY = y + 9;

  display.drawLine(cx, top, cx, bot,       SSD1306_WHITE);
  display.drawLine(lx, lowerY, rx, upperY, SSD1306_WHITE);
  display.drawLine(lx, upperY, rx, lowerY, SSD1306_WHITE);
  display.drawLine(cx, top, rx, upperY,    SSD1306_WHITE);
  display.drawLine(cx, bot, rx, lowerY,    SSD1306_WHITE);

  if (!paired) {
    display.drawLine(x, y + 12, x + 10, y, SSD1306_WHITE);
  }
}

// =============================================================
// STANDBY SCREEN
// =============================================================
void drawStandbyScreen() {
  struct tm timeinfo;
  bool timeValid = getTime(timeinfo);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  bool wifiOn = (WiFi.status() == WL_CONNECTED);
  bool btOn   = isBleConnected();

  drawWifiIcon(90, 1, wifiOn);
  drawBTIcon(112, 0, btOn);
  display.drawFastHLine(0, 17, 128, SSD1306_WHITE);

  if (!timeValid) {
    display.setTextSize(2);
    display.setCursor(16, 28);
    display.print("No Time");
    display.setTextSize(1);
    display.setCursor(18, 54);
    display.print("(WiFi needed)");
  } else {
    char timeBuf[6];
    strftime(timeBuf, sizeof(timeBuf), "%I:%M", &timeinfo);
    display.setTextSize(3);
    display.setCursor(19, 25);
    display.print(timeBuf);

    char ampm[3];
    strftime(ampm, sizeof(ampm), "%p", &timeinfo);
    display.setTextSize(1);
    display.setCursor(110, 39);
    display.print(ampm);

    char dateBuf[16];
    strftime(dateBuf, sizeof(dateBuf), "%a, %d %b", &timeinfo);
    display.setCursor(31, 56);
    display.print(dateBuf);
  }

  display.display();
}

// =============================================================
// MAIN MENU
// =============================================================
void drawMenu() {
  display.clearDisplay();

  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 0);
  display.print("MAIN MENU");

  display.drawFastHLine(0, 16, 128, SSD1306_WHITE);

  const char* labels[3] = { "1. Volume Knob", "2. Wake Mode", "3. Timer" };
  const int   yPos[3]   = { 20, 34, 48 };

  display.setTextSize(1);
  for (int i = 0; i < 3; i++) {
    if (menuSelection == i) {
      display.fillRect(0, yPos[i] - 1, 128, 10, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(6, yPos[i]);
    display.print(labels[i]);
  }

  display.setTextColor(SSD1306_WHITE);
  display.display();
}

// =============================================================
// OTHER SCREENS
// =============================================================

void drawVolumeScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("< Back (Press Btn)");
  display.setTextSize(2);
  display.setCursor(20, 28);
  display.println("VOLUME");
  display.display();
}

void drawWakeScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("< Back (Press Btn)");
  display.setTextSize(2);
  display.setCursor(4, 24);
  display.println("WAKE MODE");
  display.setTextSize(1);
  display.setCursor(14, 50);
  display.println("Mouse wiggling...");
  display.display();
}

void drawTimerSetScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Set Timer (Mins):");
  display.setTextSize(3);
  display.setCursor(40, 28);
  display.print(timerMinutes);
  display.display();
}

void drawTimerRunningScreen(int remainingSeconds) {
  int mins = remainingSeconds / 60;
  int secs = remainingSeconds % 60;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Timer Running:");
  display.setTextSize(3);
  display.setCursor(10, 28);
  if (mins < 10) display.print("0");
  display.print(mins);
  display.print(":");
  if (secs < 10) display.print("0");
  display.print(secs);
  display.display();
}

void drawTimerPausedScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Timer Paused");
  display.setTextSize(2);

  if (pauseSelection == 0) {
    display.fillRect(0, 17, 128, 20, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(10, 19);
  display.println("Resume");

  if (pauseSelection == 1) {
    display.fillRect(0, 42, 128, 20, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(10, 44);
  display.println("Stop");

  display.setTextColor(SSD1306_WHITE);
  display.display();
}

void drawTimerEndedScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 16);
  display.println("TIME UP!");
  display.setTextSize(1);
  display.setCursor(10, 50);
  display.println("Press Btn to Stop");
  display.display();
}

// =============================================================
// SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("Knobby OS — Booting");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // PHASE 1: Display + animation starts immediately on Core 0.
  // The spinning animation plays continuously in the background
  // while all the blocking init below runs on Core 1.
  initDisplayAndShowLogo();

  // PHASE 2: All blocking init — animation keeps playing throughout
  initOTA();
  connectToWiFi();
  checkForOTAUpdate();
  initWebserver();

  configTime(UTC_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
  Serial.println("Waiting for NTP time sync...");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    Serial.println("NTP synced OK.");
    cachedTimeinfo   = timeinfo;
    cachedTimeMillis = millis();
    ntpEverSynced    = true;
    lastChimeHour    = timeinfo.tm_hour;
  } else {
    Serial.println("NTP sync failed — cache helper will retry.");
  }

  initBLE();

  // PHASE 3: Stop animation cleanly, then init encoder.
  // stopWelcomeAnimation() blocks until the Core 0 task exits,
  // guaranteeing the display is free before drawMenu() uses it.
  // Encoder ISRs are attached last so they don't fire mid-init.
  stopWelcomeAnimation();
  initRotary();

  counter          = 0;
  lastActivityTime = millis();

  drawMenu();
}

// =============================================================
// HOURLY CHIME CHECK
// =============================================================
void checkHourlyChime() {
  struct tm timeinfo;
  if (!getTime(timeinfo)) return;

  int currentHour = timeinfo.tm_hour;
  if (timeinfo.tm_min == 0 && timeinfo.tm_sec == 0 && currentHour != lastChimeHour) {
    lastChimeHour = currentHour;
    playHourlyChime();
    Serial.printf("Hourly chime: %02d:00\n", currentHour);
  }
}

// =============================================================
// MAIN LOOP
// =============================================================
void loop() {
  server.handleClient();

  // ── NON-BLOCKING UI BUZZER LOGIC ────────────────────────────
  // 1. Trigger the buzzer if a button is pressed (and we aren't in the alarm state)
  if (currentState != STATE_TIMER_ENDED) {
    if (buttonLongPressed && !uiBuzzerActive) {
      digitalWrite(BUZZER_PIN, HIGH);
      uiBuzzerEndTime = millis() + 150; // 150ms for a long press
      uiBuzzerActive = true;
    } 
    else if (buttonPressed && !uiBuzzerActive) {
      digitalWrite(BUZZER_PIN, HIGH);
      uiBuzzerEndTime = millis() + 40;  // 40ms for a quick, snappy click
      uiBuzzerActive = true;
    }
  }

  // 2. Turn the buzzer off once the time has expired
  if (uiBuzzerActive && millis() >= uiBuzzerEndTime) {
    uiBuzzerActive = false;
    // Safety check: ensure the alarm hasn't triggered in the exact same millisecond
    if (currentState != STATE_TIMER_ENDED) {
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
  // ────────────────────────────────────────────────────────────

  // ── Long-press: always return to Main Menu ────────────────
  if (buttonLongPressed) {
    buttonLongPressed = false;
    buttonPressed     = false;
    digitalWrite(BUZZER_PIN, LOW);
    currentState      = STATE_MENU;
    counter           = 0;
    lastMenuSelection = -1;
    lastActivityTime  = millis();
    drawMenu();
  }

  // ── STATE: STANDBY ───────────────────────────────────────────
  if (currentState == STATE_STANDBY) {
    bool wokenByButton  = buttonPressed || buttonLongPressed;
    bool wokenByEncoder = (counter != lastStandbyCounter);

    if (wokenByButton || wokenByEncoder) {
      buttonPressed     = false;
      buttonLongPressed = false;
      currentState      = STATE_MENU;
      counter           = 0;
      lastMenuSelection = -1;
      lastActivityTime  = millis();
      switchToKeyboardMode();
      drawMenu();
      return;
    }
    if (millis() - lastStandbyUpdate >= 1000) {
      drawStandbyScreen();
      lastStandbyUpdate = millis();
    }
    delay(10);
    return;
  }

  // ── STATE: MAIN MENU ──────────────────────────────────────
  if (currentState == STATE_MENU) {
    menuSelection = abs(counter / 2) % 3;

    if (menuSelection != lastMenuSelection) {
      lastActivityTime  = millis();
      drawMenu();
      lastMenuSelection = menuSelection;
    }

    if (buttonPressed) {
      buttonPressed    = false;
      lastActivityTime = millis();
      counter          = 0;
      lastDisplayedCounter = 0;

      if (menuSelection == 0) {
        switchToKeyboardMode();
        currentState = STATE_VOLUME;
        drawVolumeScreen();
      } else if (menuSelection == 1) {
        switchToMouseMode();
        currentState    = STATE_WAKE;
        lastKeySendTime = 0;
        drawWakeScreen();
      } else if (menuSelection == 2) {
        currentState     = STATE_TIMER_SET;
        counter          = timerMinutes * 2;
        lastTimerMinutes = -1;
      }
    }

    // Enter standby after idle timeout — MENU only
    if (millis() - lastActivityTime >= STANDBY_TIMEOUT_MS) {
      currentState       = STATE_STANDBY;
      lastStandbyCounter = counter;
      lastStandbyUpdate  = 0;
    }
  }

  // ── STATE: VOLUME KNOB ────────────────────────────────────
  else if (currentState == STATE_VOLUME) {
    if (counter != lastDisplayedCounter) {
      if (counter > lastDisplayedCounter) sendVolumeUp();
      else                                sendVolumeDown();
      lastDisplayedCounter = counter;
    }
    if (buttonPressed) {
      buttonPressed     = false;
      currentState      = STATE_MENU;
      counter           = 0;
      lastMenuSelection = -1;
      lastActivityTime  = millis();
    }
  }

  // ── STATE: WAKE MODE ──────────────────────────────────────
  else if (currentState == STATE_WAKE) {
    handleWakeModeLogic();
    if (buttonPressed) {
      buttonPressed     = false;
      currentState      = STATE_MENU;
      counter           = 0;
      lastMenuSelection = -1;
      lastActivityTime  = millis();
      switchToKeyboardMode();
    }
  }

  // ── STATE: TIMER SET ──────────────────────────────────────
  else if (currentState == STATE_TIMER_SET) {
    timerMinutes = max(1, min(99, counter / 2));
    if (timerMinutes != lastTimerMinutes) {
      drawTimerSetScreen();
      lastTimerMinutes = timerMinutes;
    }
    if (buttonPressed) {
      buttonPressed        = false;
      timerRemainingMillis = timerMinutes * 60UL * 1000UL;
      timerEndTime         = millis() + timerRemainingMillis;
      currentState         = STATE_TIMER_RUNNING;
      lastDisplayUpdate    = 0;
    }
  }

  // ── STATE: TIMER RUNNING ──────────────────────────────────
  else if (currentState == STATE_TIMER_RUNNING) {
    if (millis() >= timerEndTime) {
      currentState = STATE_TIMER_ENDED;
      lastBeepTime = millis();
      beepState    = true;
      digitalWrite(BUZZER_PIN, HIGH);
      drawTimerEndedScreen();
    } else {
      if (millis() - lastDisplayUpdate >= 1000) {
        int remainingSeconds = (timerEndTime - millis()) / 1000;
        drawTimerRunningScreen(remainingSeconds);
        lastDisplayUpdate = millis();
      }
    }
    if (buttonPressed) {
      buttonPressed        = false;
      timerRemainingMillis = timerEndTime - millis();
      currentState         = STATE_TIMER_PAUSED;
      counter              = 0;
      pauseSelection       = 0;
      lastPauseSelection   = -1;
    }
  }

  // ── STATE: TIMER PAUSED ───────────────────────────────────
  else if (currentState == STATE_TIMER_PAUSED) {
    pauseSelection = abs(counter / 2) % 2;
    if (pauseSelection != lastPauseSelection) {
      drawTimerPausedScreen();
      lastPauseSelection = pauseSelection;
    }
    if (buttonPressed) {
      buttonPressed = false;
      if (pauseSelection == 0) {
        timerEndTime      = millis() + timerRemainingMillis;
        currentState      = STATE_TIMER_RUNNING;
        lastDisplayUpdate = 0;
      } else {
        currentState      = STATE_MENU;
        counter           = 0;
        lastMenuSelection = -1;
        lastActivityTime  = millis();
      }
    }
  }

  // ── STATE: TIMER ENDED (ALARM) ────────────────────────────
  else if (currentState == STATE_TIMER_ENDED) {
    if (millis() - lastBeepTime >= 500) {
      beepState = !beepState;
      digitalWrite(BUZZER_PIN, beepState ? HIGH : LOW);
      lastBeepTime = millis();
    }
    if (buttonPressed) {
      buttonPressed     = false;
      digitalWrite(BUZZER_PIN, LOW);
      currentState      = STATE_MENU;
      counter           = 0;
      lastMenuSelection = -1;
      lastActivityTime  = millis();
    }
  }

  delay(10);
}
