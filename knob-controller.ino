
#include "connectwifilogic.h"
#include "autoupdatelogic.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "rotarycode.h"



void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial time to start
  Serial.println("Software Initiated12");
  initOTA();
  connectToWiFi();
  checkForOTAUpdate();
  initRotary();
}



void loop() {
  // Only update the display if the counter value has actually changed
  if (counter != lastDisplayedCounter) {
    
    display.clearDisplay();
    
    // Print Title
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print("Counter:");

    // Print the actual counter value
    display.setTextSize(3);
    
    // Center the number roughly based on its size
    int cursorX = (counter >= 0 && counter < 10) ? 55 : 40; 
    display.setCursor(cursorX, 30);
    display.print(counter);

    display.display();
    
    // Also print to Serial Monitor for debugging
    Serial.print("Encoder Value: ");
    Serial.println(counter);

    lastDisplayedCounter = counter;
  }

  // Small delay to yield to the ESP32's background Wi-Fi/system tasks
  delay(10); 
}



