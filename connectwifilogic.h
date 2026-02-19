#ifndef CONNECT_WIFI_LOGIC
#define CONNECT_WIFI_LOGIC

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "globals.h"

// External server object defined in the main sketch or globals
extern WebServer server;
bool configReceived = false;

void startConfigPortal();

/**
 * Attempts to connect to WiFi. If it fails, starts the Config Portal.
 * This is better than going straight to Portal every time.
 */
void connectToWiFi() {
  // Initialize WiFi in Station mode first to check for saved credentials
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // This attempts to connect using credentials stored in NVS automatically

  Serial.println("Attempting to connect to saved WiFi...");
  unsigned long startAttempt = millis();
  
  // Wait 10 seconds to see if it connects automatically
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nNo saved WiFi found or connection failed. Starting Portal...");
    startConfigPortal(); 
  } else {
    Serial.println("\nConnected Automatically!");
  }
}

void startConfigPortal() {
  // Set up Access Point
  WiFi.mode(WIFI_AP_STA); // AP_STA allows scanning while being an AP
  IPAddress apIP(10, 10, 10, 10);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP("ATTENDLE_FACTORY_FIRMWARE");

  Serial.println("AP Started: ATTENDLE_FACTORY_FIRMWARE");
  Serial.println("Go to http://10.10.10.10 in your browser");

  // 1. Scan for nearby networks
  int n = WiFi.scanNetworks();
  String networkList = "<ul>";
  if (n == 0) {
    networkList += "<li>No networks found</li>";
  } else {
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      // Clickable SSID link that calls the JavaScript function
      networkList += "<li><a href='#' onclick='fillSSID(\"" + ssid + "\")'>" + ssid + "</a> (" + String(rssi) + " dBm)</li>";
    }
  }
  networkList += "</ul>";

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

  server.on("/save", []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    server.send(200, "text/html", "Credentials Received. Attempting to connect... If the device LED stops blinking, it is connected.");
    
    delay(2000);
    WiFi.softAPdisconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    
    // Check connection for 10 seconds
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
      configReceived = true; // This will break the while loop in startConfigPortal
    } else {
      // If it fails, it stays in the while(!configReceived) loop and server continues
      Serial.println("Connection failed. Stay in portal.");
      WiFi.softAP("ATTENDLE_FACTORY_FIRMWARE");
    }
  });

  server.begin();
  // Blocking loop until WiFi is successfully configured
  while (!configReceived) {
    server.handleClient();
    delay(1);
  }
  server.stop();
  Serial.println("Portal closed. Proceeding to main logic.");
}

#endif