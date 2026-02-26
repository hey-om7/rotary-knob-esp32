#ifndef AUTO_UPDATE_LOGIC_H
#define AUTO_UPDATE_LOGIC_H

#include "globals.h"
#include "customHttpLogging.h"
#include "oled_disply.h"

// EEPROM address to store version string
#define VERSION_ADDR 64
String CURRENT_VERSION;


// Save null-terminated string into EEPROM
inline void saveVersionToEEPROM(const String& version) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < version.length(); i++) {
    EEPROM.write(VERSION_ADDR + i, version[i]);
  }
  EEPROM.write(VERSION_ADDR + version.length(), '\0'); // null-terminate
  EEPROM.commit();
}

inline void eraseCompleteMemory() {
  Serial.println("Starting memory erase...");

  // 1. WIPE WI-FI CREDENTIALS (from NVS)
  // CRITICAL FIX: You must wake up the Wi-Fi radio in Station mode first!
  WiFi.mode(WIFI_STA); 
  delay(100); // Give the radio a split second to turn on

  // Now the ESP32 can safely execute the erase command without crashing
  WiFi.disconnect(true, true); 
  delay(500); 
  Serial.println("Wi-Fi credentials permanently wiped.");

  // 2. WIPE FIRMWARE VERSION (from EEPROM)
  EEPROM.begin(EEPROM_SIZE);
  // Iterate through the 32 bytes we allocated for the version string
  for (int i = 0; i < 32; i++) { 
    // 0xFF (255) is the default hardware state of empty flash memory
    EEPROM.write(VERSION_ADDR + i, 0xFF); 
  }
  EEPROM.commit();
  Serial.println("EEPROM version data wiped.");
}

inline String getVersionFromEEPROM() {
  char version[32];
  int i = 0;
  char c;
  EEPROM.begin(EEPROM_SIZE);
  do {
    c = EEPROM.read(VERSION_ADDR + i);
    // 0xFF (255) is the default state of empty flash memory. Ignore it.
    if (c == 255 || c == 0xFF) {
        break; 
    }
    version[i++] = c;
  } while (c != '\0' && i < 31);
  version[i] = '\0';
  
  return String(version);
}


inline void initOTA() {
  CURRENT_VERSION = getVersionFromEEPROM();
  
  // If the string is empty, or contains garbage data (no decimal point) from a fresh chip
  if (CURRENT_VERSION == "" || CURRENT_VERSION.indexOf('.') == -1 || CURRENT_VERSION.length() > 10) {
    Serial.println("EEPROM blank or corrupted. Setting default version 1.0.0");
    CURRENT_VERSION = "1.0.0"; 
    saveVersionToEEPROM(CURRENT_VERSION); // Format the memory properly
  } else {
    Serial.println("Loaded Firmware Version from EEPROM: " + CURRENT_VERSION);
  }
}

// Compare semver strings "x.y.z"
inline bool isVersionNewer(String current, String latest) {
  current.trim(); latest.trim();
  int curMajor = 0, curMinor = 0, curPatch = 0;
  int latMajor = 0, latMinor = 0, latPatch = 0;
  sscanf(current.c_str(), "%d.%d.%d", &curMajor, &curMinor, &curPatch);
  sscanf(latest.c_str(),  "%d.%d.%d", &latMajor, &latMinor, &latPatch);
  if (latMajor > curMajor) return true;
  if (latMajor < curMajor) return false;
  if (latMinor > curMinor) return true;
  if (latMinor < curMinor) return false;
  return latPatch > curPatch;
}

inline void checkForOTAUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    printLog("OTA: WiFi not connected, skipping update check.");
    return;
  }

  WiFiClient client;
  HTTPClient http;
  printLog("OTA: Checking for latest firmware version...");
  String mac = WiFi.macAddress();
  String versionCheckUrl = versionCheckBaseUrl + mac;
  String firmwareBinUrl = firmwareBinBaseUrl + mac;

  Serial.println("http checking for:"+versionCheckUrl);
  
  http.begin(versionCheckUrl);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    int dataIndex = payload.indexOf("\"data\":\"");
    if (dataIndex != -1) {
      int start = dataIndex + 8;
      int end = payload.indexOf("\"", start);
      String latestVersion = payload.substring(start, end);
      bool canBeUpgradable = isVersionNewer(CURRENT_VERSION, latestVersion);
      printLog("OTA: Current ver: " + CURRENT_VERSION + " Latest: " + latestVersion + " Upgradable: " + String(canBeUpgradable));
      if (canBeUpgradable) {
        printLog("OTA: Starting update...");
        httpUpdate.rebootOnUpdate(false);
        t_httpUpdate_return result = httpUpdate.update(client, firmwareBinUrl);
        switch (result) {
          case HTTP_UPDATE_FAILED:
            Serial.printf("OTA: Update failed: %s\n", httpUpdate.getLastErrorString().c_str());
            break;
          case HTTP_UPDATE_NO_UPDATES:
            printLog("OTA: No update available.");
            break;
          case HTTP_UPDATE_OK:
            saveVersionToEEPROM(latestVersion);
            updatingFirmwareScreen();
            printLog("OTA: Update successful. Rebooting...");
            delay(500);
            ESP.restart();
            break;
        }
      } else {
        printLog("OTA: Firmware up to date.");
      }
    } else {
      printLog("OTA: Failed to parse version JSON.");
    }
  } else {
    Serial.printf("OTA: Version fetch failed. HTTP code: %d\n", httpCode);
  }
  http.end();
}

#endif // AUTO_UPDATE_LOGIC_H