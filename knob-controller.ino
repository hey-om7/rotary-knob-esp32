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

// Order matters: Include rotary before your main logic so 'display' is available
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


// =============================================================
// SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Software Initiated");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  initDisplay();
  drawWelcomeScreen();

  initOTA();
  connectToWiFi();
  checkForOTAUpdate();
  initWebserver();

  configTime(UTC_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
  Serial.println("Waiting for NTP time sync...");
  
  struct tm timeinfo;
  int retryCount = 0;
  
  // Wait in 500ms increments, up to 10 seconds total (20 retries)
  while (!getLocalTime(&timeinfo, 0) && retryCount < 20) {
    Serial.print(".");
    delay(500); 
    retryCount++;
  }
  
  Serial.println();
  if (retryCount == 20) {
    Serial.println("NTP sync failed after 10 seconds — standby will show placeholder.");
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