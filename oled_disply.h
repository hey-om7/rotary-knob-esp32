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

// Forward declaration — getTime() is defined in the main sketch.
// Uses a cache so the standby clock never flickers to "No Time"
// and never blocks for 5 seconds waiting on NTP.
bool getTime(struct tm &timeinfo);



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
  // Always draw the base WiFi symbol so the icon is visible
  display.drawBitmap(cx, cy, epd_bitmap_wifi_icon, 16, 16, SSD1306_WHITE);

  // Diagonal strike-through when disconnected
  if (!connected) {
    display.drawLine(cx + 2, cy + 12, cx + 10, cy, SSD1306_WHITE);
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

// ── Lock icon (small, for status bar) ─────────────────────────
inline void drawLockIcon(int x, int y) {
  // Small padlock: 10x12 px bounding box
  // Shackle (arch)
  display.drawLine(x + 3, y + 5, x + 3, y + 2, SSD1306_WHITE);
  display.drawLine(x + 3, y + 2, x + 7, y + 2, SSD1306_WHITE);
  display.drawLine(x + 7, y + 2, x + 7, y + 5, SSD1306_WHITE);
  // Lock body
  display.fillRect(x + 1, y + 5, 9, 7, SSD1306_WHITE);
  // Keyhole
  display.fillCircle(x + 5, y + 8, 1, SSD1306_BLACK);
}

// ── Sound icon ────────────────────────────────────────────────
inline void drawSoundIcon(int x, int y) {
  int leftX = x + 2;
  int topY = y + 4;
  int height = 6;
  int width = 3;

  // Base box
  display.fillRect(leftX, topY, width, height, SSD1306_WHITE);
  // Speaker cone
  display.fillTriangle(leftX + width, topY + height / 2,
                       leftX + width + 5, topY - 3,
                       leftX + width + 5, topY + height + 2,
                       SSD1306_WHITE);
  // Sound wave 1
  display.drawFastVLine(leftX + width + 8, topY, 6, SSD1306_WHITE);
  // Sound wave 2
  display.drawFastVLine(leftX + width + 11, topY - 2, 10, SSD1306_WHITE);
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
inline void drawStandbyScreen(int yOffset = 0, bool commit = true) {
  struct tm timeinfo;
  bool timeValid = getTime(timeinfo);

  if (commit) display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ── Status icons ──────────────────────────────────────
  bool wifiOn = (WiFi.status() == WL_CONNECTED);
  bool btOn   = bleKeyboard.isConnected();

  if (enteredStandbyFromVolume) {
    drawSoundIcon(68, 1 + yOffset);
  }

  if (enteredStandbyFromDoorLock) {
    drawLockIcon(54, 1 + yOffset);
  }

  // WiFi: arc system with dot at (14, 14) → arcs open upward
  drawWifiIcon(90, 1 + yOffset, wifiOn);

  // BT: 12×18 box anchored top-right: x = 128-16 = 112, y = 0
  drawBTIcon(112, 0 + yOffset, btOn);

  // Separator
  display.drawFastHLine(0, 17 + yOffset, 128, SSD1306_WHITE);

  // ── Clock & date ──────────────────────────────────────
  if (!timeValid) {
    display.setTextSize(2);
    display.setCursor(16, 28 + yOffset);
    display.print("No Time");
    display.setTextSize(1);
    display.setCursor(18, 54 + yOffset);
    display.print("(WiFi needed)");
  } else {
    char timeBuf[6];
    strftime(timeBuf, sizeof(timeBuf), "%I:%M", &timeinfo);
    display.setTextSize(3);
    display.setCursor(19, 20+5 + yOffset);
    display.print(timeBuf);

    char ampm[3];
    strftime(ampm, sizeof(ampm), "%p", &timeinfo);
    display.setTextSize(1);
    display.setCursor(110, 34+5 + yOffset);
    display.print(ampm);

    char dateBuf[16];
    strftime(dateBuf, sizeof(dateBuf), "%a, %d %b", &timeinfo);
    display.setCursor(31, 56 + yOffset);
    display.print(dateBuf);
  }

  if (commit) display.display();
}

// =============================================================
// MAIN MENU
//
// Hierarchy rule: header BIGGER than items.
//   Header → textSize(2) = 16px tall  ("MAIN MENU", centred)
//   Items  → textSize(1) =  8px tall  (full-width highlight bar)
//
// Scrolling window: shows 3 items at a time, scrolls to keep
// the selected item visible. Original spacing preserved.
//
// 128×64 layout:
//   y  0-15 → "MAIN MENU"  textSize(2)
//   y  16   → separator
//   y  20   → item slot 1
//   y  34   → item slot 2
//   y  48   → item slot 3
// =============================================================
#define MENU_ITEM_COUNT 5
#define MENU_VISIBLE    3

inline void drawMenu(int yOffset = 0, bool commit = true) {
  if (commit) display.clearDisplay();

  // ── Header ────────────────────────────────────────────
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 0 + yOffset);
  display.print("MAIN MENU");

  display.drawFastHLine(0, 16 + yOffset, 128, SSD1306_WHITE);

  // ── Scrolling window ──────────────────────────────────
  const char* labels[MENU_ITEM_COUNT] = {
    "1. Volume Knob", "2. Wake Mode", "3. Timer",
    "4. OBS Control", "5. DoorLock"
  };
  const int yPos[MENU_VISIBLE] = { 20, 34, 48 };

  // Calculate scroll offset to keep selection visible
  int scrollTop = 0;
  if (menuSelection >= MENU_VISIBLE) {
    scrollTop = menuSelection - (MENU_VISIBLE - 1);
  }

  display.setTextSize(1);
  for (int slot = 0; slot < MENU_VISIBLE; slot++) {
    int itemIdx = scrollTop + slot;
    if (itemIdx >= MENU_ITEM_COUNT) break;

    if (itemIdx == menuSelection) {
      display.fillRect(0, yPos[slot] - 1 + yOffset, 128, 10, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(6, yPos[slot] + yOffset);
    display.print(labels[itemIdx]);
  }

  // ── Scroll indicators ─────────────────────────────────
  display.setTextColor(SSD1306_WHITE);
  if (scrollTop > 0) {
    // Up arrow indicator
    display.fillTriangle(120, 19 + yOffset, 124, 19 + yOffset, 122, 17 + yOffset, SSD1306_WHITE);
  }
  if (scrollTop + MENU_VISIBLE < MENU_ITEM_COUNT) {
    // Down arrow indicator
    display.fillTriangle(120, 57 + yOffset, 124, 57 + yOffset, 122, 59 + yOffset, SSD1306_WHITE);
  }

  display.setTextColor(SSD1306_WHITE);
  if (commit) display.display();
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

// =============================================================
// BOOT PROGRESS SCREEN
// Shows a progress bar and phase label during startup so the
// user always knows the device is alive and what it's doing.
// =============================================================
inline void drawBootProgress(const char* phase, int percent) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Title
  display.setTextSize(2);
  display.setCursor(10, 2);
  display.print("Knobby OS");

  // Separator
  display.drawFastHLine(0, 20, 128, SSD1306_WHITE);

  // Progress bar
  int barX = 10, barY = 28, barW = 108, barH = 10;
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  int fillW = ((barW - 4) * percent) / 100;
  if (fillW > 0) {
    display.fillRect(barX + 2, barY + 2, fillW, barH - 4, SSD1306_WHITE);
  }

  // Percentage
  display.setTextSize(1);
  char pctBuf[5];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", percent);
  int pctW = strlen(pctBuf) * 6;
  display.setCursor((128 - pctW) / 2, 42);
  display.print(pctBuf);

  // Phase label (centred)
  int textW = strlen(phase) * 6;
  display.setCursor((128 - textW) / 2, 54);
  display.print(phase);

  display.display();
}

// =============================================================
// CONFIG PORTAL SCREEN
// Shown on the OLED while the WiFi captive portal is active,
// so the user knows what to do and sees a countdown timer.
// =============================================================
inline void drawConfigPortalScreen(int remainingSec) {
  display.clearDisplay();

  // Header bar (inverted)
  display.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(8, 2);
  display.print("WiFi Setup Mode");

  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 16);
  display.print("Join WiFi network:");

  display.setCursor(0, 28);
  display.print("ATTENDLE_FACTORY_");
  display.setCursor(0, 37);
  display.print("  FIRMWARE");

  display.setCursor(0, 49);
  display.print("Go to: 10.10.10.10");

  // Countdown
  char buf[18];
  snprintf(buf, sizeof(buf), "Auto-skip: %ds", remainingSec);
  display.setCursor(0, 57);
  display.print(buf);

  display.display();
}

inline void drawArc(int cx, int cy, int radius, int startDeg, int endDeg, int thickness) {
  for (int r = radius; r < radius + thickness; r++) {
     for (int a = startDeg; a <= endDeg; a += 2) {
        float rad = a * PI / 180.0;
        display.drawPixel(cx + cos(rad) * r, cy + sin(rad) * r, SSD1306_WHITE);
     }
  }
}

inline void drawVolumeScreen(int yOffset = 0, bool commit = true) {
  if (commit) display.clearDisplay();
  
  // Minimal contextual text
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0 + yOffset);
  display.println("< Back");

  int cx = 64;
  int cy = 34 + yOffset;

  // Traditional side-facing speaker icon
  int rW = 8;  // Rectangle width
  int rH = 12; // Rectangle height
  int cW = 12; // Cone width
  int cH = 26; // Cone height
  int sX = cx - (rW + cW) / 2; // Leftmost X

  // Speaker base rectangle
  display.fillRect(sX, cy - rH / 2, rW, rH, SSD1306_WHITE);

  // Speaker cone (drawn as two triangles to form a trapezoid connecting to base)
  display.fillTriangle(sX + rW, cy - rH / 2,
                       sX + rW, cy + rH / 2,
                       sX + rW + cW, cy - cH / 2, SSD1306_WHITE);
  display.fillTriangle(sX + rW, cy + rH / 2,
                       sX + rW + cW, cy - cH / 2,
                       sX + rW + cW, cy + cH / 2, SSD1306_WHITE);

  if (volumeAnimIndicator != 0) {
    long elapsed = 300 - (volumeAnimTimer - millis());
    if (elapsed < 0) elapsed = 0;
    if (elapsed > 300) elapsed = 300;

    int baseRadius = 22;
    int thickness = 3 - (elapsed * 3) / 300;
    if (thickness < 1) thickness = 1;

    if (volumeAnimIndicator > 0) {
      // CW / Increase -> Right arc expanding
      int r = baseRadius + (elapsed * 10) / 300;
      drawArc(cx, cy, r, -45, 45, thickness);
      
      // Secondary trailing arc
      if (elapsed > 100) {
         int r2 = baseRadius + ((elapsed - 100) * 10) / 300;
         int thick2 = 2 - ((elapsed - 100) * 2) / 300;
         if (thick2 < 1) thick2 = 1;
         drawArc(cx, cy, r2, -35, 35, thick2);
      }
    } else {
      // CCW / Decrease -> Left arc retracting (reverse motion)
      // Reverse motion: starts far and comes inwards towards speaker
      int r = baseRadius + 10 - (elapsed * 10) / 300;
      drawArc(cx, cy, r, 135, 225, thickness);
      
      if (elapsed > 100) {
         int r2 = baseRadius + 10 - ((elapsed - 100) * 10) / 300;
         int thick2 = 2 - ((elapsed - 100) * 2) / 300;
         if (thick2 < 1) thick2 = 1;
         drawArc(cx, cy, r2, 145, 215, thick2);
      }
    }
  }

  if (commit) display.display();
}

// =============================================================
// OBS CONTROL SCREEN
//
// Clean layout:
//   y  0      → "< Back" nav hint
//   y  12     → separator
//   y  16-30  → "OBS CONTROL" header (textSize 2)
//   y  36-50  → Play/Pause icon + status label
//   y  56-63  → instruction hint
//
// obsAction: 0 = idle, 1 = play (sent 'k'), -1 = pause (sent 'j')
// =============================================================
inline void drawOBSScreen(int obsAction = 0, int yOffset = 0, bool commit = true) {
  if (commit) display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ── Nav hint ──────────────────────────────────────────
  display.setTextSize(1);
  display.setCursor(0, 0 + yOffset);
  display.print("< Back");

  // ── Separator ─────────────────────────────────────────
  display.drawFastHLine(0, 11 + yOffset, 128, SSD1306_WHITE);

  // ── Title ─────────────────────────────────────────────
  display.setTextSize(2);
  // "OBS" = 3 chars × 12px = 36px  → centered
  display.setCursor(28, 15 + yOffset);
  display.print("OBS");

  // ── Center icon area ──────────────────────────────────
  int cx = 64;
  int cy = 44 + yOffset;

  if (obsAction == 1) {
    // PLAY triangle (pointing right)
    display.fillTriangle(cx - 8, cy - 10,
                         cx - 8, cy + 10,
                         cx + 10, cy, SSD1306_WHITE);
    // Label
    display.setTextSize(1);
    display.setCursor(80, cy - 3);
    display.print("Play");
  } else if (obsAction == -1) {
    // PAUSE icon (two vertical bars)
    display.fillRect(cx - 9, cy - 9, 5, 18, SSD1306_WHITE);
    display.fillRect(cx + 4, cy - 9, 5, 18, SSD1306_WHITE);
    // Label
    display.setTextSize(1);
    display.setCursor(76, cy - 3);
    display.print("Pause");
  } else {
    // IDLE: show both icons dimmed with a slash between
    // Play triangle (smaller)
    display.drawTriangle(cx + 14, cy - 7,
                         cx + 14, cy + 7,
                         cx + 26, cy, SSD1306_WHITE);
    // Pause bars (smaller)
    display.drawRect(cx - 26, cy - 7, 4, 14, SSD1306_WHITE);
    display.drawRect(cx - 18, cy - 7, 4, 14, SSD1306_WHITE);
    // Divider slash
    display.setCursor(cx - 3, cy - 3);
    display.setTextSize(1);
    display.print("/");
  }

  // ── Bottom instruction ────────────────────────────────
  display.setTextSize(1);
  display.setCursor(4, 56 + yOffset);
  display.print("L:Pause");
  display.setCursor(76, 56 + yOffset);
  display.print("R:Play");

  if (commit) display.display();
}

// =============================================================
// DOOR LOCK CONTROL SCREEN
//
// Clean layout:
//   y  0      → "< Back" nav hint
//   y  11     → separator
//   y  14-28  → "DOOR LOCK" header (textSize 2)
//   y  34-50  → Lock/Unlock icon + status
//   y  56-63  → instruction hint
//
// doorStatus: 0 = idle, 1 = unlocked, -1 = locked, 2 = error
// =============================================================
inline void drawDoorLockScreen(int doorStatus = 0, int yOffset = 0, bool commit = true) {
  if (commit) display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ── Nav hint ──────────────────────────────────────────
  display.setTextSize(1);
  display.setCursor(0, 0 + yOffset);
  display.print("< Back");

  // ── Separator ─────────────────────────────────────────
  display.drawFastHLine(0, 11 + yOffset, 128, SSD1306_WHITE);

  // ── Title ─────────────────────────────────────────────
  display.setTextSize(2);
  display.setCursor(10, 14 + yOffset);
  display.print("DOOR LOCK");

  // ── Center icon area ──────────────────────────────────
  int cx = 64;
  int cy = 44 + yOffset;

  if (doorStatus == 1) {
    // UNLOCKED: open padlock icon
    // Lock body
    display.drawRect(cx - 10, cy - 4, 20, 14, SSD1306_WHITE);
    // Shackle (open - shifted right)
    display.drawLine(cx + 5, cy - 4, cx + 5, cy - 10, SSD1306_WHITE);
    display.drawLine(cx + 5, cy - 10, cx + 10, cy - 10, SSD1306_WHITE);
    display.drawLine(cx + 10, cy - 10, cx + 10, cy - 4, SSD1306_WHITE);
    // Label
    display.setTextSize(1);
    display.setCursor(86, cy);
    display.print("Open");
  } else if (doorStatus == -1) {
    // LOCKED: closed padlock icon
    // Lock body
    display.fillRect(cx - 10, cy - 4, 20, 14, SSD1306_WHITE);
    // Shackle (closed - centered)
    display.drawLine(cx - 5, cy - 4, cx - 5, cy - 10, SSD1306_WHITE);
    display.drawLine(cx - 5, cy - 10, cx + 5, cy - 10, SSD1306_WHITE);
    display.drawLine(cx + 5, cy - 10, cx + 5, cy - 4, SSD1306_WHITE);
    // Keyhole (black on white body)
    display.fillCircle(cx, cy + 2, 2, SSD1306_BLACK);
    // Label
    display.setTextSize(1);
    display.setCursor(82, cy);
    display.print("Locked");
  } else if (doorStatus == 2) {
    // ERROR
    display.setTextSize(1);
    display.setCursor(cx - 20, cy - 4);
    display.print("ERROR!");
    display.setCursor(cx - 30, cy + 8);
    display.print("Check WiFi");
  } else {
    // IDLE: outline padlock
    display.drawRect(cx - 10, cy - 4, 20, 14, SSD1306_WHITE);
    display.drawLine(cx - 5, cy - 4, cx - 5, cy - 10, SSD1306_WHITE);
    display.drawLine(cx - 5, cy - 10, cx + 5, cy - 10, SSD1306_WHITE);
    display.drawLine(cx + 5, cy - 10, cx + 5, cy - 4, SSD1306_WHITE);
    display.fillCircle(cx, cy + 2, 2, SSD1306_WHITE);
  }

  // ── Bottom instruction ────────────────────────────────
  display.setTextSize(1);
  display.setCursor(4, 56 + yOffset);
  display.print("L:Lock");
  display.setCursor(80, 56 + yOffset);
  display.print("R:Open");

  if (commit) display.display();
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

// =============================================================
// STOPWATCH SCREEN (HTTP-triggered, counts UP from 00:00)
//
// 128×64 layout:
//   y  0-14  → Status bar: Lock/Unlock, WiFi, BT icons + time HH:MM AM/PM
//   y  15    → separator line
//   y  18-55 → Big MM:SS stopwatch counter (textSize 3), centred
//   y  57-63 → "Stopwatch" label
// =============================================================
inline void drawStopwatchScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ── Top status bar ────────────────────────────────────
  // Lock/Unlock icon (leftmost)
  drawLockIcon(0, 1);

  // WiFi icon
  bool wifiOn = (WiFi.status() == WL_CONNECTED);
  drawWifiIcon(18, 1, wifiOn);

  // BT icon
  bool btOn = bleKeyboard.isConnected();
  drawBTIcon(38, 1, btOn);

  // Current time (HH:MM AM/PM) on right side of status bar
  struct tm timeinfo;
  bool timeValid = getTime(timeinfo);
  if (timeValid) {
    char timeBuf[6];
    strftime(timeBuf, sizeof(timeBuf), "%I:%M", &timeinfo);
    char ampm[3];
    strftime(ampm, sizeof(ampm), "%p", &timeinfo);

    display.setTextSize(1);
    // Time: right-aligned
    display.setCursor(82, 4);
    display.print(timeBuf);
    display.setCursor(113, 4);
    display.print(ampm);
  }

  // Separator
  display.drawFastHLine(0, 15, 128, SSD1306_WHITE);

  // ── Stopwatch counter (MM:SS) ─────────────────────────
  unsigned long elapsed = stopwatchElapsed;
  if (stopwatchRunning) {
    elapsed += (millis() - stopwatchStartMillis);
  }
  int totalSec = elapsed / 1000;
  int mins = totalSec / 60;
  int secs = totalSec % 60;

  display.setTextSize(3);
  char swBuf[6];
  snprintf(swBuf, sizeof(swBuf), "%02d:%02d", mins, secs);
  // textSize 3 = 18px wide per char, 5 chars + colon = ~90px → center at ~19
  display.setCursor(19, 25);
  display.print(swBuf);

  // ── Bottom label ──────────────────────────────────────
  display.setTextSize(1);
  display.setCursor(34, 56);
  display.print("Focus Mode");

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