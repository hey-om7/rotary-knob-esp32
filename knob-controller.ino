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

// BUZZER_PIN is defined in globals.h

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
// SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("Knobby OS — Booting");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // PHASE 1: Display init — must come first so we can show progress
  initDisplay();
  drawBootProgress("Starting up...", 5);

  // PHASE 2: Read firmware version from EEPROM
  drawBootProgress("Reading firmware...", 10);
  initOTA();

  // PHASE 3: WiFi connection (with config portal fallback)
  drawBootProgress("Connecting WiFi...", 20);
  bool wifiOk = connectToWiFi();

  if (!wifiOk) {
    // Launch the captive portal and show instructions on the OLED.
    // If no one configures WiFi within the timeout, continue offline.
    setupConfigPortal();
    unsigned long portalStart = millis();
    int lastSec = -1;
    while (!handleConfigPortalClient() &&
           millis() - portalStart < CONFIG_PORTAL_TIMEOUT_MS) {
      int remainingSec = (int)((CONFIG_PORTAL_TIMEOUT_MS - (millis() - portalStart)) / 1000);
      if (remainingSec != lastSec) {
        drawConfigPortalScreen(remainingSec);
        lastSec = remainingSec;
      }
      delay(10);
    }
    stopConfigPortal();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi configured via portal!");
    } else {
      Serial.println("Portal timed out. Continuing in offline mode.");
    }
  }

  wifiConnectedAtBoot = (WiFi.status() == WL_CONNECTED);

  // PHASE 4: OTA firmware update check
  drawBootProgress("Checking updates...", 50);
  checkForOTAUpdate();

  // PHASE 5: Web server + mDNS
  drawBootProgress("Starting services...", 65);
  initWebserver();

  // PHASE 6: NTP time sync (skip if offline)
  if (WiFi.status() == WL_CONNECTED) {
    drawBootProgress("Syncing clock...", 75);
    configTime(UTC_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
    Serial.println("Waiting for NTP time sync...");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
      Serial.println("NTP synced OK.");
      cachedTimeinfo   = timeinfo;
      cachedTimeMillis = millis();
      ntpEverSynced    = true;
      lastChimeHour    = timeinfo.tm_hour;
    } else {
      Serial.println("NTP sync failed — cache helper will retry.");
    }
  } else {
    Serial.println("No WiFi — skipping NTP sync.");
  }

  // PHASE 7: BLE keyboard
  drawBootProgress("Starting BLE...", 90);
  initBLE();

  // PHASE 8: Rotary encoder (ISRs attached last to avoid mid-init firing)
  drawBootProgress("Ready!", 100);
  initRotary();
  delay(400); // Brief pause so the user sees "Ready!"

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
  checkHourlyChime();

  // ── WiFi auto-reconnect (non-blocking) ──────────────────────
  {
    static unsigned long lastWifiCheck = 0;
    if (wifiConnectedAtBoot &&
        WiFi.status() != WL_CONNECTED &&
        millis() - lastWifiCheck >= WIFI_RECONNECT_INTERVAL_MS) {
      lastWifiCheck = millis();
      Serial.println("WiFi lost. Attempting reconnect...");
      WiFi.disconnect();
      WiFi.begin();
    }
  }

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
    enteredStandbyFromVolume = false;
    enteredStandbyFromDoorLock = false;
    enteredStandbyFromOBS = false;
    drawMenu();
  }

  // ── STATE: STANDBY ───────────────────────────────────────────
  if (currentState == STATE_STANDBY) {
    bool wokenByButton  = buttonPressed || buttonLongPressed;
    bool wokenByEncoder = (counter != lastStandbyCounter);

    if (wokenByButton || wokenByEncoder) {
      buttonPressed     = false;
      buttonLongPressed = false;
      
      currentState      = STATE_ANIMATING_WAKE;
      animYOffset       = 0;
      animLastFrameTime = millis();
      preAnimState      = STATE_STANDBY;
      
      if (enteredStandbyFromVolume) {
          postAnimState = STATE_VOLUME;
      } else if (enteredStandbyFromDoorLock) {
          postAnimState = STATE_DOORLOCK;
      } else if (enteredStandbyFromOBS) {
          postAnimState = STATE_OBS;
      } else {
          postAnimState = STATE_MENU;
      }
      
      if (postAnimState == STATE_MENU) {
          counter = 0;
      } else {
          // If returning to volume/doorlock, wait till animation ends to read new changes,
          // but we preserve lastStandbyCounter to prevent jumping.
          counter = lastStandbyCounter;
      }
      
      lastMenuSelection = -1;
      lastActivityTime  = millis();
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
    menuSelection = abs(counter / 2) % MENU_ITEM_COUNT;

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
        currentState    = STATE_WAKE;
        lastKeySendTime = millis();
        drawWakeScreen();
      } else if (menuSelection == 2) {
        currentState     = STATE_TIMER_SET;
        counter          = timerMinutes * 2;
        lastTimerMinutes = -1;
      } else if (menuSelection == 3) {
        currentState = STATE_OBS;
        counter      = 0;
        lastDisplayedCounter = 0;
        obsKeySent       = false;
        obsLastDirection  = 0;
        obsLastRotateTime = millis();
        drawOBSScreen(0);
      } else if (menuSelection == 4) {
        currentState = STATE_DOORLOCK;
        counter      = 0;
        lastDisplayedCounter = 0;
        doorKeySent       = false;
        doorLastDirection  = 0;
        doorLastRotateTime = millis();
        doorLastStatus     = 0;
        drawDoorLockScreen(0);
      }
    }

    // Enter standby after idle timeout — MENU
    if (millis() - lastActivityTime >= STANDBY_TIMEOUT_MS) {
      currentState       = STATE_ANIMATING_TO_STANDBY;
      animYOffset        = 0;
      animLastFrameTime  = millis();
      preAnimState       = STATE_MENU;
      postAnimState      = STATE_STANDBY;
      enteredStandbyFromVolume = false;
      enteredStandbyFromDoorLock = false;
      enteredStandbyFromOBS = false;
      lastStandbyCounter = counter;
      lastStandbyUpdate  = 0;
    }
  }

  // ── STATE: VOLUME KNOB ────────────────────────────────────
  else if (currentState == STATE_VOLUME) {
    if (counter != lastDisplayedCounter) {
      if (counter > lastDisplayedCounter) {
          sendVolumeUp();
          volumeAnimIndicator = 1;
      } else {
          sendVolumeDown();
          volumeAnimIndicator = -1;
      }
      lastDisplayedCounter = counter;
      lastActivityTime = millis();
      volumeAnimTimer = millis() + 300; // animation duration
      drawVolumeScreen();
    }
    
    // Clear or play volume animation after timeout
    if (volumeAnimIndicator != 0) {
      if (millis() >= volumeAnimTimer) {
        volumeAnimIndicator = 0;
        drawVolumeScreen();
      } else if (millis() - lastVolAnimFrameTime >= 20) { 
        lastVolAnimFrameTime = millis();
        drawVolumeScreen();
      }
    }
    if (buttonPressed) {
      buttonPressed     = false;
      currentState      = STATE_MENU;
      counter           = 0;
      lastMenuSelection = -1;
      lastActivityTime  = millis();
    }

    // Inactivity timeout logic for Volume Screen
    if (millis() - lastActivityTime >= STANDBY_TIMEOUT_MS) {
      currentState       = STATE_ANIMATING_TO_STANDBY;
      animYOffset        = 0;
      animLastFrameTime  = millis();
      preAnimState       = STATE_VOLUME;
      postAnimState      = STATE_STANDBY;
      enteredStandbyFromVolume = true;
      enteredStandbyFromDoorLock = false;
      enteredStandbyFromOBS = false;
      lastStandbyCounter = counter;
      lastStandbyUpdate  = 0;
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

  // ── STATE: OBS CONTROL ────────────────────────────────────
  else if (currentState == STATE_OBS) {
    // Reset after 500ms of no rotation
    if (obsKeySent && millis() - obsLastRotateTime >= 500) {
      obsKeySent       = false;
      obsLastDirection = 0;
      drawOBSScreen(0);
    }

    if (counter != lastDisplayedCounter) {
      int direction = (counter > lastDisplayedCounter) ? 1 : -1;
      obsLastRotateTime = millis();

      // Only send the combo once until 500ms idle reset
      if (!obsKeySent) {
        obsKeySent       = true;
        obsLastDirection = direction;

        if (bleKeyboard.isConnected()) {
          if (direction == 1) {
            // Knob RIGHT → CMD+OPTION+SHIFT+K (Play)
            bleKeyboard.press(KEY_LEFT_GUI);
            bleKeyboard.press(KEY_LEFT_ALT);
            bleKeyboard.press(KEY_LEFT_SHIFT);
            bleKeyboard.press('k');
            delay(20);
            bleKeyboard.releaseAll();
            Serial.println("OBS: Sent Cmd+Opt+Shift+K (Play)");
          } else {
            // Knob LEFT → CMD+OPTION+SHIFT+J (Pause)
            bleKeyboard.press(KEY_LEFT_GUI);
            bleKeyboard.press(KEY_LEFT_ALT);
            bleKeyboard.press(KEY_LEFT_SHIFT);
            bleKeyboard.press('j');
            delay(20);
            bleKeyboard.releaseAll();
            Serial.println("OBS: Sent Cmd+Opt+Shift+J (Pause)");
          }
        }
        drawOBSScreen(direction);
      }

      lastDisplayedCounter = counter;
      lastActivityTime     = millis();
    }

    if (buttonPressed) {
      buttonPressed     = false;
      obsKeySent        = false;
      obsLastDirection  = 0;
      currentState      = STATE_MENU;
      counter           = 0;
      lastMenuSelection = -1;
      lastActivityTime  = millis();
      enteredStandbyFromOBS = false;
    }

    // Inactivity timeout logic for OBS Screen
    if (millis() - lastActivityTime >= STANDBY_TIMEOUT_MS) {
      currentState       = STATE_ANIMATING_TO_STANDBY;
      animYOffset        = 0;
      animLastFrameTime  = millis();
      preAnimState       = STATE_OBS;
      postAnimState      = STATE_STANDBY;
      enteredStandbyFromOBS      = true;
      enteredStandbyFromVolume   = false;
      enteredStandbyFromDoorLock = false;
      lastStandbyCounter = counter;
      lastStandbyUpdate  = 0;
    }
  }

  // ── STATE: DOOR LOCK CONTROL ──────────────────────────────
  else if (currentState == STATE_DOORLOCK) {
    // Reset after 500ms of no rotation
    if (doorKeySent && millis() - doorLastRotateTime >= 500) {
      doorKeySent       = false;
      doorLastDirection  = 0;
    }

    if (counter != lastDisplayedCounter) {
      int direction = (counter > lastDisplayedCounter) ? 1 : -1;
      doorLastRotateTime = millis();

      // Only send the HTTP request once until 500ms idle reset
      if (!doorKeySent) {
        doorKeySent       = true;
        doorLastDirection  = direction;

        if (WiFi.status() == WL_CONNECTED) {
          HTTPClient http;
          int httpCode;

          if (direction == 1) {
            // Knob RIGHT → Open/Unlock
            http.begin("http://doorlock.local/open?password=149311&api=true");
            http.setTimeout(5000);
            httpCode = http.GET();
            http.end();

            if (httpCode > 0 && httpCode < 400) {
              doorLastStatus = 1;
              Serial.println("DoorLock: Opened");
            } else {
              doorLastStatus = 2;
              Serial.printf("DoorLock: Open failed (%d)\n", httpCode);
            }
          } else {
            // Knob LEFT → Lock
            http.begin("http://doorlock.local/setMode?password=149311&mode=locked");
            http.setTimeout(5000);
            httpCode = http.GET();
            http.end();

            if (httpCode > 0 && httpCode < 400) {
              doorLastStatus = -1;
              Serial.println("DoorLock: Locked");
            } else {
              doorLastStatus = 2;
              Serial.printf("DoorLock: Lock failed (%d)\n", httpCode);
            }
          }
        } else {
          doorLastStatus = 2;
          Serial.println("DoorLock: No WiFi");
        }

        drawDoorLockScreen(doorLastStatus);
      }

      lastDisplayedCounter = counter;
      lastActivityTime     = millis();
    }

    if (buttonPressed) {
      buttonPressed      = false;
      doorKeySent        = false;
      doorLastDirection   = 0;
      doorLastStatus     = 0;
      currentState       = STATE_MENU;
      counter            = 0;
      lastMenuSelection  = -1;
      lastActivityTime   = millis();
      enteredStandbyFromDoorLock = false;
    }

    // Inactivity timeout logic for DoorLock Screen
    if (millis() - lastActivityTime >= STANDBY_TIMEOUT_MS) {
      currentState       = STATE_ANIMATING_TO_STANDBY;
      animYOffset        = 0;
      animLastFrameTime  = millis();
      preAnimState       = STATE_DOORLOCK;
      postAnimState      = STATE_STANDBY;
      enteredStandbyFromDoorLock = true;
      enteredStandbyFromVolume   = false;
      enteredStandbyFromOBS      = false;
      lastStandbyCounter = counter;
      lastStandbyUpdate  = 0;
    }
  }



  // ── STATE: ANIMATING TO STANDBY ───────────────────────────
  else if (currentState == STATE_ANIMATING_TO_STANDBY) {
    if (millis() - animLastFrameTime >= 15) { 
      animYOffset += 6; 
      if (animYOffset >= 64) {
        animYOffset = 64;
        currentState = postAnimState;
        drawStandbyScreen();
      } else {
        display.clearDisplay();
        if (preAnimState == STATE_VOLUME) {
          drawVolumeScreen(animYOffset, false);
        } else if (preAnimState == STATE_DOORLOCK) {
          drawDoorLockScreen(doorLastStatus, animYOffset, false);
        } else if (preAnimState == STATE_OBS) {
          drawOBSScreen(obsLastDirection, animYOffset, false);
        } else {
          drawMenu(animYOffset, false);
        }
        drawStandbyScreen(animYOffset - 64, false);
        display.display();
      }
      animLastFrameTime = millis();
    }
  }

  // ── STATE: ANIMATING WAKE ─────────────────────────────────
  else if (currentState == STATE_ANIMATING_WAKE) {
    if (millis() - animLastFrameTime >= 15) {
      animYOffset -= 8; 
      if (animYOffset <= -64) {
        animYOffset = 0;
        currentState = postAnimState;
        if (postAnimState == STATE_VOLUME) {
          drawVolumeScreen();
        } else if (postAnimState == STATE_DOORLOCK) {
          drawDoorLockScreen(doorLastStatus);
        } else if (postAnimState == STATE_OBS) {
          drawOBSScreen(obsLastDirection);
        } else {
          enteredStandbyFromVolume = false;
          enteredStandbyFromDoorLock = false;
          enteredStandbyFromOBS = false;
          drawMenu();
        }
      } else {
        display.clearDisplay();
        drawStandbyScreen(animYOffset, false); 
        if (postAnimState == STATE_VOLUME) {
          drawVolumeScreen(animYOffset + 64, false); 
        } else if (postAnimState == STATE_DOORLOCK) {
          drawDoorLockScreen(doorLastStatus, animYOffset + 64, false);
        } else if (postAnimState == STATE_OBS) {
          drawOBSScreen(obsLastDirection, animYOffset + 64, false);
        } else {
          drawMenu(animYOffset + 64, false);
        }
        display.display();
      }
      animLastFrameTime = millis();
    }
  }

  delay(10);
}
