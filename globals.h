#ifndef GLOBALS_H
#define GLOBALS_H

#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <EEPROM.h>

// Remote logging over MQTT -> FirmwareManager console (Devices -> Logging).
// Use RLOGE / RLOGW / RLOGI / RLOGD(tag, fmt, ...) instead of Serial.print.
// The broker protocol is the same for every device model (topics
// fm/<mac>/log|status|cmd, client id fm-<mac>). The server labels stored lines
// with this device's model number from its Devices registration, so nothing
// model-specific is configured here. Optional local-only Serial prefix:
// #define RLOG_SERIAL_PREFIX "[KNOB]"
#include "RemoteLog.h"

// --- FirmwareManager server --------------------------------------------------
// OTA: try the Render deployment first; if it can't be reached (or the update
// from it fails) fall back to the local server.
#define FM_PRIMARY_BASE_URL  "https://firmwaremanager.onrender.com"
const char* ipServer = "192.168.1.100";
const String fmFallbackBaseUrl = "http://" + String(ipServer) + ":8080";

const char* FM_VERSION_PATH  = "/api/v1/device/firmware/version?macAddress=";
const char* FM_FIRMWARE_PATH = "/api/v1/device/firmware?macAddress=";

// Render free instances can take a while to wake up; give the first request time.
#define OTA_PRIMARY_TIMEOUT_MS  20000
#define OTA_FALLBACK_TIMEOUT_MS 5000

// Remote logging (MQTT broker built into FirmwareManager)
// Render:    "wss://firmwaremanager.onrender.com/mqtt"
// Local dev: "ws://192.168.1.100:8080/mqtt"
#define MQTT_BROKER_URI "wss://firmwaremanager.onrender.com/mqtt"
#define MQTT_USERNAME   "firmwaremanager"   // = server MQTT_USERNAME (same for every device model)
#define MQTT_PASSWORD   "change-me"         // = server MQTT_PASSWORD

#define EEPROM_SIZE 1024
#define BUZZER_PIN  5

#define STANDBY_TIMEOUT_MS         10000UL  // Standby mode after 10 seconds
#define CONFIG_PORTAL_TIMEOUT_MS   60000UL  // 60s timeout for WiFi config portal
#define WIFI_RECONNECT_INTERVAL_MS 10000UL  // 10s between manual WiFi reconnect attempts


const unsigned long wakeModeKeyInterval = 2 * 60 * 1000; // 2 minutes

const char* hostname = "knobcontroller"; // For mDNS: http://knobcontroller.local


// --- State Machine Definitions ---
enum AppState {
  STATE_MENU,
  STATE_STANDBY,       // Auto-activates after idle
  STATE_VOLUME,
  STATE_WAKE,
  STATE_TIMER_SET,
  STATE_TIMER_RUNNING,
  STATE_TIMER_PAUSED,
  STATE_TIMER_ENDED,
  STATE_OBS,           // OBS Play/Pause control
  STATE_DOORLOCK,      // Door Lock/Unlock control
  STATE_STOPWATCH,     // Stopwatch triggered via HTTP endpoint
  STATE_ANIMATING_TO_STANDBY,
  STATE_ANIMATING_WAKE
};

AppState currentState = STATE_MENU;
int menuSelection     = 0;
int lastMenuSelection = -1;

int pauseSelection     = 0;
int lastPauseSelection = -1;

// --- Timer Variables ---
int timerMinutes      = 1;
int lastTimerMinutes  = -1;
unsigned long timerEndTime         = 0;
unsigned long timerRemainingMillis = 0;

// --- Standby Tracking ---
bool enteredStandbyFromVolume = false;
bool enteredStandbyFromDoorLock = false;
bool enteredStandbyFromOBS = false;

// --- OBS Control Tracking ---
bool obsKeySent              = false;   // true once combo is sent for current rotation
int  obsLastDirection        = 0;       // 1 = right, -1 = left, 0 = idle
unsigned long obsLastRotateTime = 0;    // millis() of last knob movement

// --- DoorLock Control Tracking ---
bool doorKeySent              = false;
int  doorLastDirection        = 0;
unsigned long doorLastRotateTime = 0;
int  doorLastStatus           = 0;      // 0=idle, 1=unlocked, -1=locked, 2=error

// --- Stopwatch Variables (HTTP-triggered) ---
bool stopwatchRunning       = false;   // true while counting up
unsigned long stopwatchStartMillis = 0; // millis() when started
unsigned long stopwatchElapsed     = 0; // accumulated elapsed ms

// --- WiFi Tracking ---
bool wifiConnectedAtBoot = false;

// --- Animation Tracking ---
int animYOffset = 0;
unsigned long animLastFrameTime = 0;
AppState postAnimState = STATE_STANDBY;
AppState preAnimState  = STATE_MENU;
int volumeAnimIndicator = 0;
unsigned long volumeAnimTimer = 0;
unsigned long lastVolAnimFrameTime = 0;

#endif