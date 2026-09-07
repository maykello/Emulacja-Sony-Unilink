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
    virtual void flush() override;

    bool isWiFiConnected() const { return wifiConnected; }
    bool hasClient() const { return clientConnected; }

    // Zapisz bufor kołowy logów do pliku na pendrive
    void dumpBlackbox();

private:
    WiFiServer server{WIFI_LOGGER_PORT};
    WiFiClient activeClient;
    bool wifiConnected = false;
    bool clientConnected = false;
    unsigned long lastReconnectAttempt = 0;

    // Bufor kołowy (czarna skrzynka) przechowujący ostatnie ~4KB logów
    static constexpr size_t BLACKBOX_SIZE = 4096;
    char blackboxBuf[BLACKBOX_SIZE];
    size_t blackboxHead = 0;
    size_t blackboxTail = 0;
    bool blackboxFull = false;

    void addToBlackbox(uint8_t c);
};

extern WiFiLoggerClass WiFiLogger;

#endif // WIFI_LOGGER_H
