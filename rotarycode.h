#ifndef ROTARY_CODE_LOGIC
#define ROTARY_CODE_LOGIC


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
// Marked 'volatile' because they are modified inside Interrupt Service Routines (ISRs)
volatile int counter = 0;
volatile int lastClk = HIGH;
volatile unsigned long lastButtonPress = 0;

int lastDisplayedCounter = -9999; // Force initial screen update

// --- Interrupt Service Routine for Encoder ---
// IRAM_ATTR keeps the function in RAM for fast execution on ESP32
void IRAM_ATTR readEncoder() {
  int clkValue = digitalRead(ENCODER_CLK);
  int dtValue = digitalRead(ENCODER_DT);
  
  if (clkValue != lastClk) {
    // Determine direction by comparing CLK and DT states
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
  unsigned long currentTime = millis();
  // 200ms debounce to prevent multiple triggers from one press
  if (currentTime - lastButtonPress > 200) { 
    counter = 0; // Reset the counter to 0 on button press
    lastButtonPress = currentTime;
  }
}

void initRotary(){
   // 1. Initialize custom I2C pins for the ESP32-C3
  Wire.begin(I2C_SDA, I2C_SCL);

  // 2. Initialize the OLED display
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed. Check wiring!"));
    for(;;); // Halt execution if OLED is not found
  }
  
  // Show startup screen
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 25);
  display.println("ESP32-C3 Ready!");
  display.display();
  delay(1500);

  // 3. Setup Encoder Pins
  // Using INPUT_PULLUP in case your encoder module doesn't have built-in pull-up resistors
  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  lastClk = digitalRead(ENCODER_CLK);

  // 4. Attach Interrupts
  // This tells the ESP32 to monitor these pins automatically in the background
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_SW), readButton, FALLING);
}

#endif