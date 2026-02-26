#ifndef GLOBALS_H
#define GLOBALS_H

#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <EEPROM.h>

#define EEPROM_SIZE 1024

#define STANDBY_TIMEOUT_MS 10000UL //Standby mode after 10 seconds

const char* ipServer = "192.168.1.100";

const String versionCheckBaseUrl = "http://" + String(ipServer) + ":8080/api/v1/device/firmware/version?macAddress=" ;
const String firmwareBinBaseUrl = "http://" + String(ipServer) +":8080/api/v1/device/firmware?macAddress=";
const String baseLoggingUrl = "http://" + String(ipServer) + ":8080/api/v1/device/logs";


const unsigned long wakeModeKeyInterval = 2 * 60 * 1000; // 2 minutes

const String hostname = "knobcontroller"; // For mDNS: http://knobcontroller.local


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

int pauseSelection     = 0;
int lastPauseSelection = -1;

// --- Timer Variables ---
int timerMinutes      = 1;
int lastTimerMinutes  = -1;
unsigned long timerEndTime         = 0;
unsigned long timerRemainingMillis = 0;


#endif