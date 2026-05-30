#ifndef CONNECT_WIFI_LOGIC
#define CONNECT_WIFI_LOGIC

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "globals.h"

// External server object defined in the main sketch
extern WebServer server;
bool configReceived = false;

/**
 * Attempts to connect to WiFi using saved credentials.
 * Returns true if connected, false if the config portal is needed.
 */
bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // Uses credentials stored in NVS automatically

  Serial.println("Attempting to connect to saved WiFi...");
  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected Automatically!");
    return true;
  }

  Serial.println("\nNo saved WiFi found or connection failed.");
  return false;
}

/**
 * Initializes the captive portal AP and registers web routes.
 * NON-BLOCKING — call handleConfigPortalClient() in a loop.
 */
void setupConfigPortal() {
  configReceived = false;

  WiFi.mode(WIFI_AP_STA);
  IPAddress apIP(10, 10, 10, 10);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP("ATTENDLE_FACTORY_FIRMWARE");

  Serial.println("AP Started: ATTENDLE_FACTORY_FIRMWARE");
  Serial.println("Go to http://10.10.10.10 in your browser");

  // Scan for nearby networks
  int n = WiFi.scanNetworks();
  String networkList = "<ul>";
  if (n == 0) {
    networkList += "<li>No networks found</li>";
  } else {
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      networkList += "<li><a href='#' onclick='fillSSID(\"" + ssid + "\")'>" + ssid + "</a> (" + String(rssi) + " dBm)</li>";
    }
  }
  networkList += "</ul>";

  // --- Portal landing page ---
  server.on("/", [networkList]() {
    String mac = WiFi.macAddress();
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>WiFi Config</title>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <style>
    body { font-family: Arial; background-color: #f4f4f4; padding: 20px; }
    .container { background: #fff; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); max-width: 400px; margin: auto; }
    h2 { text-align: center; color: #333; }
    .mac { text-align: center; color: #666; font-size: 0.8em; margin-bottom: 20px; }
    .networks { background: #eee; padding: 10px; border-radius: 5px; max-height: 150px; overflow-y: auto; margin-bottom: 20px; }
    .networks ul { list-style: none; padding: 0; }
    .networks li { padding: 5px 0; border-bottom: 1px solid #ddd; }
    .networks a { color: #28a745; text-decoration: none; font-weight: bold; }
    input { width: 100%; padding: 10px; margin: 10px 0; border: 1px solid #ccc; border-radius: 5px; box-sizing: border-box; }
    input[type='submit'] { background: #28a745; color: white; border: none; cursor: pointer; font-size: 16px; }
    input[type='submit']:hover { background: #218838; }
  </style>
  <script>
    function fillSSID(ssid) {
      document.getElementById('ssid').value = ssid;
    }
  </script>
</head>
<body>
  <div class='container'>
    <h2>WiFi Setup</h2>
    <div class='mac'>Device MAC: )rawliteral" + mac + R"rawliteral(</div>
    <h4>Available Networks (Click to select):</h4>
    <div class='networks'>)rawliteral" + networkList + R"rawliteral(</div>
    <form action='/save' method='POST'>
      <input type='text' name='ssid' id='ssid' placeholder='SSID' required>
      <input type='password' name='pass' placeholder='Password' required>
      <input type='submit' value='Save & Connect'>
    </form>
  </div>
</body>
</html>)rawliteral";
    server.send(200, "text/html", html);
  });

  // --- Credential save handler ---
  server.on("/save", []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    server.send(200, "text/html",
      "Credentials received. Connecting...<br>"
      "If successful, the device will continue automatically.");

    delay(1000);

    // Try connecting with new credentials
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected via portal!");
      configReceived = true;
    } else {
      Serial.println("Connection failed. Restarting portal AP...");
      WiFi.mode(WIFI_AP_STA);
      IPAddress apIP(10, 10, 10, 10);
      IPAddress netMsk(255, 255, 255, 0);
      WiFi.softAPConfig(apIP, apIP, netMsk);
      WiFi.softAP("ATTENDLE_FACTORY_FIRMWARE");
    }
  });

  server.begin();
}

/**
 * Call in a loop — returns true once WiFi is configured via the portal.
 */
bool handleConfigPortalClient() {
  server.handleClient();
  return configReceived;
}

/**
 * Tears down the config portal AP and stops the portal server.
 */
void stopConfigPortal() {
  server.stop();
  WiFi.softAPdisconnect(true);
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.mode(WIFI_STA);   // Keep WiFi, drop AP
  }
  Serial.println("Config portal closed.");
}

#endif