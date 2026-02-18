#ifndef GLOBALS_H
#define GLOBALS_H

#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <EEPROM.h>
#include <WebServer.h>

// Define the server object here so it actually exists in memory
// This fixes the "undefined reference" error
#ifndef SERVER_DEFINITION
#define SERVER_DEFINITION
WebServer server(80); 
#endif

const char* ipServer = "192.168.1.100";
// URL is better built dynamically to ensure it has the latest MAC
#endif