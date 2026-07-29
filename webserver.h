#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <ESPmDNS.h>   
#include "globals.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

extern WebServer server;  // Declare server if defined elsewhere

extern Adafruit_SSD1306 display;

// Externs needed for stopwatch endpoint handlers
extern unsigned long lastActivityTime;
extern volatile int counter;
extern void drawMenu(int yOffset, bool commit);

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
    Serial.print("mDNS responder started: http://");
    Serial.print(hostname);
    Serial.println(".local");
  }

  // Override any lingering config portal routes
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "Knobby OS Running");
  });
  server.on("/save", HTTP_POST, []() {
    server.send(404, "text/plain", "Not Found");
  });

  // Register API endpoints
  server.on("/api/restart", HTTP_GET, handleRestart);

  // Stopwatch endpoints
  server.on("/api/stopwatch/start", HTTP_GET, []() {
    stopwatchElapsed     = 0;
    stopwatchStartMillis = millis();
    stopwatchRunning     = true;
    currentState         = STATE_STOPWATCH;
    server.send(200, "application/json", "{\"status\":\"stopwatch_started\"}");
    Serial.println("Stopwatch: Started via HTTP");
  });

  server.on("/api/stopwatch/stop", HTTP_GET, []() {
    stopwatchRunning     = false;
    stopwatchElapsed     = 0;
    currentState         = STATE_MENU;
    counter              = 0;
    lastMenuSelection    = -1;
    lastActivityTime     = millis();
    drawMenu(0, true);
    server.send(200, "application/json", "{\"status\":\"stopwatch_stopped\"}");
    Serial.println("Stopwatch: Stopped via HTTP, back to menu");
  });

  server.begin();
  Serial.println("HTTP server started.");
}

#endif