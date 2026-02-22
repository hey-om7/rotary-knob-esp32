#include "connectwifilogic.h"
#include "autoupdatelogic.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Order matters: Include rotary before your main logic so 'display' is available
#include "rotarycode.h"
#include "blelogic.h" 

#define BUZZER_PIN 5

// --- State Machine Definitions ---
enum AppState {
  STATE_MENU,
  STATE_VOLUME,
  STATE_WAKE,
  STATE_TIMER_SET,
  STATE_TIMER_RUNNING,
  STATE_TIMER_PAUSED,
  STATE_TIMER_ENDED
};

AppState currentState = STATE_MENU;
int menuSelection = 0; // 0 = Volume Knob, 1 = Wake Mode, 2 = Timer
int lastMenuSelection = -1;

// --- Timer Variables ---
int timerMinutes = 1;
int lastTimerMinutes = -1;
unsigned long timerEndTime = 0;
unsigned long timerRemainingMillis = 0;
int pauseSelection = 0; // 0 = Resume, 1 = Stop
int lastPauseSelection = -1;
unsigned long lastDisplayUpdate = 0;
unsigned long lastBeepTime = 0;
bool beepState = false;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Software Initiated");

  // Initialize Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  initOTA();
  connectToWiFi();
  checkForOTAUpdate();

  initBLE();
  initRotary();
  
  counter = 0; // Reset counter for clean startup
}

// ---------------------------------------------------------
// UI DRAWING FUNCTIONS
// ---------------------------------------------------------

void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(10, 0);
  display.println("--- MAIN MENU ---");

  // 1. Volume Knob
  if (menuSelection == 0) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); // Invert text
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(10, 15);
  display.println("1. Volume Knob");

  // 2. Wake Mode
  if (menuSelection == 1) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); // Invert text
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(10, 30);
  display.println("2. Wake Mode");

  // 3. Timer
  if (menuSelection == 2) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); // Invert text
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(10, 45);
  display.println("3. Timer");

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

void drawTimerSetScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Set Timer (Mins):");
  
  display.setTextSize(3);
  display.setCursor(40, 30);
  display.print(timerMinutes);
  display.display();
}

void drawTimerRunningScreen(int remainingSeconds) {
  int mins = remainingSeconds / 60;
  int secs = remainingSeconds % 60;
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Timer Running...");
  
  display.setTextSize(3);
  display.setCursor(20, 30);
  if (mins < 10) display.print("0");
  display.print(mins);
  display.print(":");
  if (secs < 10) display.print("0");
  display.print(secs);
  display.display();
}

void drawTimerPausedScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Timer Paused");

  if (pauseSelection == 0) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(20, 25);
  display.println("1. Resume");

  if (pauseSelection == 1) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(20, 45);
  display.println("2. Stop");
  display.display();
}

void drawTimerEndedScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(15, 20);
  display.println("TIME UP!");
  display.setTextSize(1);
  display.setCursor(10, 50);
  display.println("Press Btn to Stop");
  display.display();
}

// ---------------------------------------------------------
// MAIN LOOP
// ---------------------------------------------------------

void loop() {
  // ---------------------------------------------------------
  // GLOBAL OVERRIDE: LONG PRESS (Return to Main Menu)
  // ---------------------------------------------------------
  if (buttonLongPressed) {
    buttonLongPressed = false;
    buttonPressed = false;         // Clear any accidental short presses
    
    digitalWrite(BUZZER_PIN, LOW); // Silence the buzzer in case the alarm was ringing
    
    currentState = STATE_MENU;
    counter = 0;
    lastMenuSelection = -1;        // Force an immediate menu redraw
  }


  // ---------------------------------------------------------
  // STATE: MAIN MENU
  // ---------------------------------------------------------
  if (currentState == STATE_MENU) {
    menuSelection = abs(counter / 2) % 3;
    
    if (menuSelection != lastMenuSelection) {
      drawMenu();
      lastMenuSelection = menuSelection;
    }

    if (buttonPressed) {
      buttonPressed = false;
      counter = 0; 
      lastDisplayedCounter = 0;
      
      if (menuSelection == 0) {
        currentState = STATE_VOLUME;
        drawVolumeScreen();
      } else if (menuSelection == 1) {
        currentState = STATE_WAKE;
        drawWakeScreen();
      } else if (menuSelection == 2) {
        currentState = STATE_TIMER_SET;
        counter = timerMinutes * 2; 
        lastTimerMinutes = -1; 
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

    if (buttonPressed) { 
      buttonPressed = false;
      currentState = STATE_MENU;
      counter = 0;
      lastMenuSelection = -1; 
    }
  }

  // ---------------------------------------------------------
  // STATE: WAKE MODE
  // ---------------------------------------------------------
  else if (currentState == STATE_WAKE) {
    handleWakeModeLogic();
    if (buttonPressed) { 
      buttonPressed = false;
      currentState = STATE_MENU;
      counter = 0;
      lastMenuSelection = -1; 
    }
  }

  // ---------------------------------------------------------
  // STATE: TIMER SET
  // ---------------------------------------------------------
  else if (currentState == STATE_TIMER_SET) {
    timerMinutes = max(1, min(99, counter / 2)); 
    
    if (timerMinutes != lastTimerMinutes) {
      drawTimerSetScreen();
      lastTimerMinutes = timerMinutes;
    }

    if (buttonPressed) { 
      buttonPressed = false;
      timerRemainingMillis = timerMinutes * 60UL * 1000UL;
      timerEndTime = millis() + timerRemainingMillis;
      
      currentState = STATE_TIMER_RUNNING;
      lastDisplayUpdate = 0; 
    }
  }

  // ---------------------------------------------------------
  // STATE: TIMER RUNNING
  // ---------------------------------------------------------
  else if (currentState == STATE_TIMER_RUNNING) {
    if (millis() >= timerEndTime) {
      currentState = STATE_TIMER_ENDED;
      lastBeepTime = millis();
      beepState = true;
      digitalWrite(BUZZER_PIN, HIGH);
      drawTimerEndedScreen();
    } else {
      if (millis() - lastDisplayUpdate >= 1000) {
        int remainingSeconds = (timerEndTime - millis()) / 1000;
        drawTimerRunningScreen(remainingSeconds);
        lastDisplayUpdate = millis();
      }
    }

    if (buttonPressed) { 
      buttonPressed = false;
      timerRemainingMillis = timerEndTime - millis(); 
      
      currentState = STATE_TIMER_PAUSED;
      counter = 0; 
      pauseSelection = 0;
      lastPauseSelection = -1; 
    }
  }

  // ---------------------------------------------------------
  // STATE: TIMER PAUSED
  // ---------------------------------------------------------
  else if (currentState == STATE_TIMER_PAUSED) {
    pauseSelection = abs(counter / 2) % 2; 
    
    if (pauseSelection != lastPauseSelection) {
      drawTimerPausedScreen();
      lastPauseSelection = pauseSelection;
    }

    if (buttonPressed) {
      buttonPressed = false;
      if (pauseSelection == 0) {
        timerEndTime = millis() + timerRemainingMillis;
        currentState = STATE_TIMER_RUNNING;
        lastDisplayUpdate = 0; 
      } else {
        currentState = STATE_MENU;
        counter = 0;
        lastMenuSelection = -1;
      }
    }
  }

  // ---------------------------------------------------------
  // STATE: TIMER ENDED (ALARM)
  // ---------------------------------------------------------
  else if (currentState == STATE_TIMER_ENDED) {
    if (millis() - lastBeepTime >= 500) {
      beepState = !beepState;
      digitalWrite(BUZZER_PIN, beepState ? HIGH : LOW);
      lastBeepTime = millis();
    }

    if (buttonPressed) { 
      buttonPressed = false;
      digitalWrite(BUZZER_PIN, LOW); 
      currentState = STATE_MENU;
      counter = 0;
      lastMenuSelection = -1;
    }
  }

  delay(10);
}