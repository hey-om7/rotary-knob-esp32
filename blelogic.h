#ifndef BLE_LOGIC_H
#define BLE_LOGIC_H

#include <BleKeyboard.h>

BleKeyboard bleKeyboard("ESP32-C3 Knob", "Domestic Labs", 100);

unsigned long lastKeySendTime = 0;
const unsigned long keyInterval = 5000; // 5 seconds

void initBLE() {
  bleKeyboard.begin();
  
  // Use working BLE stack
  BLESecurity *pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
  pSecurity->setCapability(ESP_IO_CAP_NONE);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
}

char generateRandomLetter() {
  int randomNumber = random(0, 26); 
  return 'a' + randomNumber; 
}

void handleWakeModeLogic() {
  if (bleKeyboard.isConnected()) { 
    if (millis() - lastKeySendTime >= keyInterval) { 
      char randomLetter = generateRandomLetter();
      
      bleKeyboard.write(randomLetter);
      Serial.print("Sent key: ");
      Serial.println(randomLetter);
      
      lastKeySendTime = millis();
    }
  }
}

void sendVolumeUp() {
  if (bleKeyboard.isConnected()) {
    bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
    Serial.println("Volume Up");
  }
}

void sendVolumeDown() {
  if (bleKeyboard.isConnected()) {
    bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
    Serial.println("Volume Down");
  }
}

#endif