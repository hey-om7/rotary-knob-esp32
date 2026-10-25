#ifndef AUTO_UPDATE_LOGIC_H
#define AUTO_UPDATE_LOGIC_H

#include "globals.h"
#include <WiFiClientSecure.h>
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
  RLOGW("SYS", "Starting memory erase...");

  // 1. WIPE WI-FI CREDENTIALS (from NVS)
  // CRITICAL FIX: You must wake up the Wi-Fi radio in Station mode first!
  WiFi.mode(WIFI_STA); 
  delay(100); // Give the radio a split second to turn on

  // Now the ESP32 can safely execute the erase command without crashing
  WiFi.disconnect(true, true); 
  delay(500); 
  RLOGW("SYS", "Wi-Fi credentials permanently wiped.");

  // 2. WIPE FIRMWARE VERSION (from EEPROM)
  EEPROM.begin(EEPROM_SIZE);
  // Iterate through the 32 bytes we allocated for the version string
  for (int i = 0; i < 32; i++) { 
    // 0xFF (255) is the default hardware state of empty flash memory
    EEPROM.write(VERSION_ADDR + i, 0xFF); 
  }
  EEPROM.commit();
  RLOGW("SYS", "EEPROM version data wiped.");
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
    RLOGW("OTA", "EEPROM blank or corrupted. Setting default version 1.0.0");
    CURRENT_VERSION = "1.0.0"; 
    saveVersionToEEPROM(CURRENT_VERSION); // Format the memory properly
  } else {
    RLOGI("OTA", "Loaded firmware version from EEPROM: %s", CURRENT_VERSION.c_str());
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

// Outcome of one OTA attempt against one server
enum OtaAttempt {
  OTA_SERVER_UNAVAILABLE,  // version check failed (unreachable / bad response) -> try next server
  OTA_UP_TO_DATE,          // server answered and we're current -> stop
  OTA_UPDATE_FAILED        // newer version found but download/flash failed -> try next server
};

// Root CA bundle compiled into the ESP-IDF core (same one RemoteLog uses for wss://).
// Verifies Render's HTTPS certificate without pasting a PEM.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

inline OtaAttempt tryOTAFromServer(const char* label, const String& baseUrl, uint16_t timeoutMs) {
  const bool https = baseUrl.startsWith("https://");
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  if (https) {
    secureClient.setCACertBundle(x509_crt_bundle_start);
    secureClient.setHandshakeTimeout(timeoutMs / 1000);
  }
  WiFiClient& client = https ? static_cast<WiFiClient&>(secureClient) : plainClient;

  const String mac = WiFi.macAddress();
  const String versionCheckUrl = baseUrl + FM_VERSION_PATH + mac;
  const String firmwareBinUrl  = baseUrl + FM_FIRMWARE_PATH + mac;

  RLOGI("OTA", "[%s] Checking for latest firmware version...", label);
  RLOGD("OTA", "[%s] Version check URL: %s", label, versionCheckUrl.c_str());

  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, versionCheckUrl)) {
    RLOGW("OTA", "[%s] Invalid URL: %s", label, versionCheckUrl.c_str());
    return OTA_SERVER_UNAVAILABLE;
  }
  int httpCode = http.GET();
  if (httpCode != 200) {
    RLOGW("OTA", "[%s] Version fetch failed: %d (%s)", label, httpCode,
          httpCode < 0 ? HTTPClient::errorToString(httpCode).c_str() : "HTTP status");
    http.end();
    return OTA_SERVER_UNAVAILABLE;
  }
  String payload = http.getString();
  http.end();

  int dataIndex = payload.indexOf("\"data\":\"");
  if (dataIndex == -1) {
    RLOGE("OTA", "[%s] Failed to parse version JSON: %s", label, payload.c_str());
    return OTA_SERVER_UNAVAILABLE;
  }
  int start = dataIndex + 8;
  int end = payload.indexOf("\"", start);
  String latestVersion = payload.substring(start, end);
  bool canBeUpgradable = isVersionNewer(CURRENT_VERSION, latestVersion);
  RLOGI("OTA", "[%s] Current ver: %s Latest: %s Upgradable: %d", label,
        CURRENT_VERSION.c_str(), latestVersion.c_str(), canBeUpgradable);
  if (!canBeUpgradable) {
    RLOGI("OTA", "[%s] Firmware up to date.", label);
    return OTA_UP_TO_DATE;
  }

  RLOGI("OTA", "[%s] Starting update to %s...", label, latestVersion.c_str());
  updatingFirmwareScreen();   // Show progress BEFORE the download starts
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  RemoteLog::pause(true);     // Keep the network free for the download; lines keep buffering
  t_httpUpdate_return result = httpUpdate.update(client, firmwareBinUrl);
  RemoteLog::pause(false);

  switch (result) {
    case HTTP_UPDATE_OK:
      saveVersionToEEPROM(latestVersion);
      RLOGI("OTA", "[%s] Update to %s successful. Rebooting...", label, latestVersion.c_str());
      RemoteLog::flush(3000);  // Get the result to the console before we reboot
      ESP.restart();
      return OTA_UP_TO_DATE;   // not reached
    case HTTP_UPDATE_NO_UPDATES:
      RLOGI("OTA", "[%s] No update available.", label);
      return OTA_UP_TO_DATE;
    case HTTP_UPDATE_FAILED:
    default:
      RLOGE("OTA", "[%s] Update failed (%d): %s", label,
            httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      return OTA_UPDATE_FAILED;
  }
}

// Try Render first; only fall back to the local server if Render couldn't be
// used (unreachable, bad response, or the update itself failed).
inline void checkForOTAUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    RLOGW("OTA", "WiFi not connected, skipping update check.");
    return;
  }

  if (tryOTAFromServer("render", FM_PRIMARY_BASE_URL, OTA_PRIMARY_TIMEOUT_MS) == OTA_UP_TO_DATE) return;

  RLOGW("OTA", "Render unavailable or update failed - falling back to %s", fmFallbackBaseUrl.c_str());
  tryOTAFromServer("local", fmFallbackBaseUrl, OTA_FALLBACK_TIMEOUT_MS);
}

#endif // AUTO_UPDATE_LOGIC_H