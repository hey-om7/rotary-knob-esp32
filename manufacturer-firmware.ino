
#include "connectwifilogic.h"
#include "autoupdatelogic.h"


void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial time to start
  Serial.println("Software Initiated");
  connectToWiFi();
}



void loop() {
  checkForOTAUpdate();
  delay(10000);
}



