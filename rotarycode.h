#ifndef ROTARY_CODE_LOGIC
#define ROTARY_CODE_LOGIC

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

// --- OLED Configuration ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C

// Custom I2C Pins for ESP32-C3
#define I2C_SDA 8
#define I2C_SCL 9

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Rotary Encoder Pins ---
#define ENCODER_CLK 2
#define ENCODER_DT  3
#define ENCODER_SW  4

// --- Variables ---
volatile int counter = 0;
volatile int lastClk = HIGH;

volatile unsigned long buttonDownTime = 0;
volatile bool buttonPressed = false;     // Short press flag
volatile bool buttonLongPressed = false; // Long press flag

int lastDisplayedCounter = -9999; 

// --- Interrupt Service Routine for Encoder ---
void IRAM_ATTR readEncoder() {
  int clkValue = digitalRead(ENCODER_CLK);
  int dtValue = digitalRead(ENCODER_DT);
  
  if (clkValue != lastClk) {
    if (clkValue != dtValue) {
      counter++;
    } else {
      counter--;
    }
    lastClk = clkValue;
  }
}

// --- Interrupt Service Routine for Button ---
void IRAM_ATTR readButton() {
  int btnState = digitalRead(ENCODER_SW);
  unsigned long currentTime = millis();
  
  if (btnState == LOW) { 
    // Only record the time if a press isn't already being tracked
    if (buttonDownTime == 0) {
      buttonDownTime = currentTime;
    }
  } else { 
    // Button fully released
    if (buttonDownTime > 0) {
      unsigned long pressDuration = currentTime - buttonDownTime;
      
      if (pressDuration >= 2000) { 
        buttonLongPressed = true;
      } else if (pressDuration > 20) { // Lowered to 20ms for quick taps
        buttonPressed = true; 
      }
      
      // Reset the timer so it's ready for the next press
      buttonDownTime = 0; 
    }
  }
}

void initRotary(){
  

  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  lastClk = digitalRead(ENCODER_CLK);

  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), readEncoder, CHANGE);
  
  // CRITICAL FIX: Track CHANGE (both press and release) instead of just FALLING
  attachInterrupt(digitalPinToInterrupt(ENCODER_SW), readButton, CHANGE);
}

#endif