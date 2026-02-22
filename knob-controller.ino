#include "connectwifilogic.h"
#include "autoupdatelogic.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// Order matters: Include rotary before your main logic so 'display' is available
#include "rotarycode.h"
#include "blelogic.h"

#define BUZZER_PIN 5

// --- NTP Configuration ---
// IST (India/Pune) = UTC+5:30 = 19800 seconds
#define NTP_SERVER      "pool.ntp.org"
#define UTC_OFFSET_SEC  19800
#define DST_OFFSET_SEC  0

// --- State Machine Definitions ---
enum AppState {
  STATE_MENU,
  STATE_STANDBY,       // Auto-activates after 1 min idle on menu
  STATE_VOLUME,
  STATE_WAKE,
  STATE_TIMER_SET,
  STATE_TIMER_RUNNING,
  STATE_TIMER_PAUSED,
  STATE_TIMER_ENDED
};

AppState currentState = STATE_MENU;
int menuSelection     = 0;
int lastMenuSelection = -1;

// --- Standby Tracking ---
#define STANDBY_TIMEOUT_MS 6000UL
unsigned long lastActivityTime  = 0;
unsigned long lastStandbyUpdate = 0;
int lastStandbyCounter          = 0;

// --- Timer Variables ---
int timerMinutes      = 1;
int lastTimerMinutes  = -1;
unsigned long timerEndTime         = 0;
unsigned long timerRemainingMillis = 0;
int pauseSelection     = 0;
int lastPauseSelection = -1;
unsigned long lastDisplayUpdate = 0;
unsigned long lastBeepTime      = 0;
bool beepState = false;


// =============================================================
// ICON DRAWING  (pure Adafruit GFX primitives, no bitmaps)
// =============================================================

// ── WiFi icon ─────────────────────────────────────────────────
// Classic 3-arc + dot symbol.
// cx, cy = centre of the arc system (dot position).
// connected = all 3 arcs;  disconnected = 1 small arc only.
void drawWifiIcon(int cx, int cy, bool connected) {
  // 1. Draw a larger central dot (radius 3 instead of 2)
  display.fillCircle(cx, cy, 3, SSD1306_WHITE);

  // 2. Draw the thick arcs
  if (connected) {
    // Inner thick arc: Draw three consecutive thin arcs to create thickness
    for (int r = 7; r <= 9; r++) {
      display.drawCircleHelper(cx, cy, r, 0x03, SSD1306_WHITE);
    }
    // Outer thick arc: Draw another set of three thin arcs
    for (int r = 13; r <= 15; r++) {
      display.drawCircleHelper(cx, cy, r, 0x03, SSD1306_WHITE);
    }
  } else {
    // For "not connected", draw only the inner thick arc and the dot
    for (int r = 7; r <= 9; r++) {
      display.drawCircleHelper(cx, cy, r, 0x03, SSD1306_WHITE);
    }
  }

  // 3. Masking: Slice off the sides to create the cone shape.
  // We need larger masks to cover the new, thicker, and wider arcs.
  // The mask starts just above the central dot (cy - 3) so it doesn't cut it.
  display.fillTriangle(cx, cy - 3, cx - 22, cy - 3, cx - 22, cy - 25, SSD1306_BLACK);
  display.fillTriangle(cx, cy - 3, cx + 22, cy - 3, cx + 22, cy - 25, SSD1306_BLACK);

  // 4. Redraw the center dot last to ensure it's perfectly clean on top
  display.fillCircle(cx, cy, 3, SSD1306_WHITE);
}

// ── Bluetooth icon ────────────────────────────────────────────
// Classic ᛒ kite shape: vertical spine + two mirrored kite arms.
// x, y = top-left corner of a ~12×18 px bounding box.
// paired = full symbol;  unpaired = spine only.
void drawBTIcon(int x, int y, bool paired) {
  int cx = x + 5;         // Center spine X
  int top = y;            // Top of spine Y
  int bot = y + 12;       // Bottom of spine Y

  int lx = x + 1;         // Leftmost boundary
  int rx = x + 9;         // Rightmost boundary
  int upperY = y + 3;     // Upper intersection Y
  int lowerY = y + 9;     // Lower intersection Y

  // Draw the core Bluetooth runes
  display.drawLine(cx, top, cx, bot, SSD1306_WHITE);       // Vertical Spine
  display.drawLine(lx, lowerY, rx, upperY, SSD1306_WHITE); // Left-bottom to Right-top
  display.drawLine(lx, upperY, rx, lowerY, SSD1306_WHITE); // Left-top to Right-bottom
  display.drawLine(cx, top, rx, upperY, SSD1306_WHITE);    // Top to Right-top
  display.drawLine(cx, bot, rx, lowerY, SSD1306_WHITE);    // Bottom to Right-bottom

  // Visual indicator for "not paired"
  if (!paired) {
    // Draws a diagonal strike-through to indicate disconnection
    display.drawLine(x, y + 12, x + 10, y, SSD1306_WHITE);
  }
}

// =============================================================
// STANDBY SCREEN
// 128×64 layout:
//   y  0-16  → WiFi icon (left)  +  BT icon (right)
//   y  17    → separator line
//   y  20-43 → HH:MM  textSize(3) — pixel-perfect centred
//   y  46-53 → AM/PM  textSize(1) — centred
//   y  56-63 → "Www, DD Mmm"  textSize(1) — centred
// =============================================================
void drawStandbyScreen() {
  struct tm timeinfo;
  bool timeValid = getLocalTime(&timeinfo);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ── Status icons ──────────────────────────────────────
  bool wifiOn = (WiFi.status() == WL_CONNECTED);
  bool btOn   = bleKeyboard.isConnected();

  // WiFi: arc system with dot at (14, 14) → arcs open upward
  drawWifiIcon(14, 14, wifiOn);

  // BT: 12×18 box anchored top-right: x = 128-16 = 112, y = 0
  drawBTIcon(112, 0, btOn);

  // Separator
  display.drawFastHLine(0, 17, 128, SSD1306_WHITE);

  // ── Clock & date ──────────────────────────────────────
  if (!timeValid) {
    display.setTextSize(2);
    display.setCursor(16, 28);
    display.print("No Time");
    display.setTextSize(1);
    display.setCursor(18, 54);
    display.print("(WiFi needed)");
  } else {
    // Time — 12-hour format, zero-padded → always 5 chars "HH:MM"
    // textSize(3): char width=18px, height=24px
    // 5 chars × 18px = 90px  →  x = (128-90)/2 = 19
    char timeBuf[6];
    strftime(timeBuf, sizeof(timeBuf), "%I:%M", &timeinfo);
    display.setTextSize(3);
    display.setCursor(19, 20+5);
    display.print(timeBuf);

    // AM/PM — textSize(1): 2 chars × 6px = 12px  →  x = (128-12)/2 = 58
    char ampm[3];
    strftime(ampm, sizeof(ampm), "%p", &timeinfo);
    display.setTextSize(1);
    display.setCursor(110, 34+5);
    display.print(ampm);

    // Date — "Mon, 23 Feb" = 11 chars × 6px = 66px  →  x = (128-66)/2 = 31
    char dateBuf[16];
    strftime(dateBuf, sizeof(dateBuf), "%a, %d %b", &timeinfo);
    display.setCursor(31, 56);
    display.print(dateBuf);
  }

  display.display();
}

// =============================================================
// MAIN MENU
//
// Hierarchy rule: header BIGGER than items.
//   Header → textSize(2) = 16px tall  ("MAIN MENU", centred)
//   Items  → textSize(1) =  8px tall  (full-width highlight bar)
//
// 128×64 layout:
//   y  0-15 → "MAIN MENU"  textSize(2)
//   y  16   → separator
//   y  20-27 → item 1
//   y  34-41 → item 2
//   y  48-55 → item 3
// =============================================================
void drawMenu() {
  display.clearDisplay();

  // ── Header ────────────────────────────────────────────
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  // "MAIN MENU" = 9 chars × 12px = 108px  →  x = (128-108)/2 = 10
  display.setCursor(10, 0);
  display.print("MAIN MENU");

  display.drawFastHLine(0, 16, 128, SSD1306_WHITE);

  // ── Items ─────────────────────────────────────────────
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
// OTHER SCREENS  (unchanged logic, minor y-position tidying)
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
  display.setCursor(20, 50);
  display.println("Sending keys...");
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
  display.println("Timer Running...");
  display.setTextSize(3);
  display.setCursor(20, 28);
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
  delay(1000);
  Serial.println("Software Initiated");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  initOTA();
  connectToWiFi();
  checkForOTAUpdate();

  configTime(UTC_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
  Serial.println("Waiting for NTP time sync...");
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) {
    Serial.println("NTP sync failed — standby will show placeholder.");
  } else {
    Serial.println("NTP synced OK.");
  }

  initBLE();
  initRotary();

  counter          = 0;
  lastActivityTime = millis();
}

// =============================================================
// MAIN LOOP
// =============================================================
void loop() {

  // ── Long-press: always return to Main Menu ────────────────
  if (buttonLongPressed) {
    buttonLongPressed = false;
    buttonPressed     = false;
    digitalWrite(BUZZER_PIN, LOW);
    currentState      = STATE_MENU;
    counter           = 0;
    lastMenuSelection = -1;
    lastActivityTime  = millis();
  }

  // ── STATE: STANDBY ────────────────────────────────────────
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
        currentState = STATE_VOLUME;
        drawVolumeScreen();
      } else if (menuSelection == 1) {
        currentState = STATE_WAKE;
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
