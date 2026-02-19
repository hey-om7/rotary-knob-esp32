#ifndef HTTPLOGGING_H
#define HTTPLOGGING_H

#include "globals.h"

bool REMOTE_LOGGING = false;


// -------------------
// HTTP logging function
// -------------------
int loggHttp(const String &value) {
  HTTPClient http;


  http.setTimeout(2000);  // 2-second timeout (recommended)
  http.begin(baseLoggingUrl);
  http.addHeader("Content-Type", "text/plain");

  int httpCode = http.POST(value);

  if (httpCode == 200) {
    Serial.println("Successfully logged: " + value);
  } else {
    Serial.println("Error logging: " + String(httpCode));
  }

  http.end();
  return httpCode;
}

// -------------------
// Unified print function
// -------------------
inline void printLog(const String &msg) {
  if (REMOTE_LOGGING) {
    int r = loggHttp(msg);
    // Optional: do something if r != 200
  } else {
    Serial.println(msg);
  }
}

inline void printLogForce(const String &msg) {
    int r = loggHttp(msg);
}

#endif // GLOBALS_H