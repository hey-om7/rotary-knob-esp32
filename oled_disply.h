#ifndef OLED_DISPLY_H
#define OLED_DISPLY_H

#include "connectwifilogic.h"
#include "autoupdatelogic.h"
#include "webserver.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <WebServer.h>
#include "globals.h"
#include "rotarycode.h"
#include "blelogic.h"

#define BUZZER_PIN 5



// =============================================================
// ICON DRAWING  (pure Adafruit GFX primitives, no bitmaps)
// =============================================================

// ── WiFi icon ─────────────────────────────────────────────────

// 'wifi-icon', 30x30px
const unsigned char epd_bitmap_wifi_icon [] PROGMEM = {
	0x03, 0x80, 0x0f, 0xf0, 0x3f, 0xfc, 0xfc, 0x3f, 0x61, 0x8e, 0x0f, 0xf0, 0x1f, 0xf8, 0x0e, 0x70, 
	0x00, 0x00, 0x03, 0xc0, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Classic 3-arc + dot symbol.
// cx, cy = centre of the arc system (dot position).
// connected = all 3 arcs;  disconnected = 1 small arc only.
inline void drawWifiIcon(int cx, int cy, bool connected) {
  if (connected) {
    display.drawBitmap(cx, cy, epd_bitmap_wifi_icon, 16, 16, SSD1306_WHITE);
  } 
    // Visual indicator for "not paired"
    // Draws a diagonal strike-through to indicate disconnection
    if(!connected){
      display.drawLine(cx+2, cy + 12, cx + 10, cy, SSD1306_WHITE);
    }
}

// ── Bluetooth icon ────────────────────────────────────────────
// Classic ᛒ kite shape: vertical spine + two mirrored kite arms.
// x, y = top-left corner of a ~12×18 px bounding box.
// paired = full symbol;  unpaired = spine only.
inline void drawBTIcon(int x, int y, bool paired) {
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
inline void drawStandbyScreen() {
  struct tm timeinfo;
  bool timeValid = getLocalTime(&timeinfo);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ── Status icons ──────────────────────────────────────
  bool wifiOn = (WiFi.status() == WL_CONNECTED);
  bool btOn   = bleKeyboard.isConnected();

  // WiFi: arc system with dot at (14, 14) → arcs open upward
  drawWifiIcon(90, 1, wifiOn);

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
inline void drawMenu() {
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

inline void drawWelcomeScreen(){
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 15);
  display.println("Knobby OS");
  display.setTextSize(1);
  display.setCursor(15, 50);
  display.println("Initializing...");
  display.display();
}

inline void drawVolumeScreen() {
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

inline void drawWakeScreen() {
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

inline void drawTimerRunningScreen(int remainingSeconds) {
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

inline void drawTimerPausedScreen() {
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

inline void drawTimerEndedScreen() {
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

// --- UI Buzzer Variables ---
unsigned long uiBuzzerEndTime = 0;
bool uiBuzzerActive = false;

inline void initDisplay(){
  Wire.begin(I2C_SDA, I2C_SCL);

  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed. Check wiring!"));
    for(;;); 
  }
  delay(100);
}

inline void updatingFirmwareScreen() {
  display.clearDisplay();

  // --- Header Bar ---
  display.fillRect(0, 0, 128, 14, SSD1306_WHITE);     // Solid title bar
  display.setTextColor(SSD1306_BLACK);                // Inverted text for bar
  display.setTextSize(1);
  display.setCursor(4, 3);
  display.println("Firmware Update");

  // --- Main Message ---
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 24);
  display.setTextSize(2);
  display.println("Updating");

  // --- Sub Message / Status ---
  display.setTextSize(1);
  display.setCursor(10, 48);
  display.println("Please wait...");

  // --- Decorative Loading Bar (static or animated externally) ---
  display.drawRect(10, 58, 108, 4, SSD1306_WHITE);     // outline
  // To animate: draw filled rect during update routine
  // display.fillRect(12, 60, progressWidth, 2, SSD1306_WHITE);

  display.display();
}


#endif