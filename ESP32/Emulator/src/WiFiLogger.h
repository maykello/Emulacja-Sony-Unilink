#ifndef WIFI_LOGGER_H
#define WIFI_LOGGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

constexpr uint16_t WIFI_LOGGER_PORT = 12345;
constexpr const char* MDNS_HOSTNAME = "unilink";

class WiFiLoggerClass : public Print {
public:
    void begin(unsigned long baud = 921600);
    void loop();

    
    // Print implementation to send to both Serial and WiFi TCP client
    virtual size_t write(uint8_t c) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;

    bool isWiFiConnected() const { return wifiConnected; }
    bool hasClient() const { return clientConnected; }

private:
    WiFiServer server{WIFI_LOGGER_PORT};
    WiFiClient activeClient;
    bool wifiConnected = false;
    bool clientConnected = false;
    unsigned long lastReconnectAttempt = 0;
};

extern WiFiLoggerClass WiFiLogger;

#endif // WIFI_LOGGER_H
