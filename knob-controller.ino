#include "connectwifilogic.h"
#include "autoupdatelogic.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Order matters: Include rotary before your main logic so 'display' is available
#include "rotarycode.h"
#include "blelogic.h" 

// --- State Machine Definitions ---
enum AppState {
  STATE_MENU,
  STATE_VOLUME,
  STATE_WAKE
};

AppState currentState = STATE_MENU;
int menuSelection = 0; // 0 = Volume Knob, 1 = Wake Mode
int lastMenuSelection = -1;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Software Initiated");

  initOTA();
  connectToWiFi();
  checkForOTAUpdate();

  initBLE();
  initRotary();
  
  counter = 0; // Reset counter for clean startup
}

void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(10, 5);
  display.println("--- MAIN MENU ---");

  // Highlight logic
  if (menuSelection == 0) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); // Invert text for highlight
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(10, 25);
  display.println("1. Volume Knob");

  if (menuSelection == 1) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); // Invert text for highlight
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(10, 40);
  display.println("2. Wake Mode");

  display.display();
}

void drawVolumeScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("< Back (Press Btn)");
  
  display.setTextSize(2);
  display.setCursor(20, 30);
  display.println("VOLUME");
  display.display();
}

void drawWakeScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("< Back (Press Btn)");
  
  display.setTextSize(2);
  display.setCursor(10, 30);
  display.println("WAKE MODE");
  display.setTextSize(1);
  display.setCursor(20, 50);
  display.println("Sending keys...");
  display.display();
}

void loop() {
  // ---------------------------------------------------------
  // STATE: MAIN MENU
  // ---------------------------------------------------------
  if (currentState == STATE_MENU) {
    // Math to keep the selection between 0 and 1 cleanly
    menuSelection = abs(counter / 2) % 2; 

    if (menuSelection != lastMenuSelection) {
      drawMenu();
      lastMenuSelection = menuSelection;
    }

    if (buttonPressed) {
      buttonPressed = false;
      counter = 0; // Reset rotary counter for the new mode
      lastDisplayedCounter = 0;
      
      if (menuSelection == 0) {
        currentState = STATE_VOLUME;
        drawVolumeScreen();
      } else {
        currentState = STATE_WAKE;
        drawWakeScreen();
      }
    }
  }

  // ---------------------------------------------------------
  // STATE: VOLUME KNOB
  // ---------------------------------------------------------
  else if (currentState == STATE_VOLUME) {
    if (counter != lastDisplayedCounter) {
      if (counter > lastDisplayedCounter) {
        sendVolumeUp();
      } else if (counter < lastDisplayedCounter) {
        sendVolumeDown();
      }
      lastDisplayedCounter = counter;
    }

    if (buttonPressed) { // Back to menu
      buttonPressed = false;
      currentState = STATE_MENU;
      counter = 0;
      lastMenuSelection = -1; // Force menu redraw
    }
  }

  // ---------------------------------------------------------
  // STATE: WAKE MODE
  // ---------------------------------------------------------
  else if (currentState == STATE_WAKE) {
    handleWakeModeLogic();

    if (buttonPressed) { // Back to menu
      buttonPressed = false;
      currentState = STATE_MENU;
      counter = 0;
      lastMenuSelection = -1; // Force menu redraw
    }
  }

  delay(10);
}