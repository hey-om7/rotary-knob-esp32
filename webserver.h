#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <ESPmDNS.h>   
#include "globals.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

extern WebServer server;  // Declare server if defined elsewhere


extern Adafruit_SSD1306 display;

inline void handleRestart() {
  // Send the HTTP response first so the client isn't left hanging
  server.send(200, "application/json", "{\"status\":\"restarting\"}");
  
  // 3. Update the OLED Display
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(5, 25);
  display.println("Restarting");
  display.display();

  // Give the user a moment to read the screen and the network buffer time to send the response
  delay(1000); 
  
  ESP.restart();
}

inline void initWebserver() {
  // Start mDNS
  if (!MDNS.begin(hostname)) {     // hostname = knobcontroller.local
    Serial.println("Error setting up MDNS!");
  } else {
    Serial.println("mDNS responder started: http://"+hostname+".local");
  }

  // Register API endpoint
  server.on("/api/restart", HTTP_GET, handleRestart);

  server.begin();
  Serial.println("HTTP server started.");
}

#endif