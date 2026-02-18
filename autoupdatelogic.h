#ifndef AUTO_UPDATE_LOGIC
#define AUTO_UPDATE_LOGIC

#include "globals.h"

#define VERSION_ADDR 64
String CURRENT_VERSION;



void checkForOTAUpdate() {
  WiFiClient client;
  Serial.println("Checking for OTA update...");
  
  String mac = WiFi.macAddress();
  mac.replace(":", "%3A");
  String url = "http://"+String(ipServer)+":8080/api/v1/device/firmware?macAddress=" + mac;

  // ESP32 syntax: httpUpdate.update(client, url)
  t_httpUpdate_return result = httpUpdate.update(client, url);

  switch (result) {
    case HTTP_UPDATE_FAILED:
    Serial.println(url);
      Serial.printf("OTA Update failed: (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("No OTA update available.");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("OTA Update successful. Rebooting...");
      delay(1000);
      ESP.restart();
      break;
  }
}

#endif