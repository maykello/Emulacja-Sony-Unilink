#ifndef WIFI_LOGGER_H
#define WIFI_LOGGER_H

#include <Arduino.h>
#include <WiFi.h>
// Flaga globalna: włącz/wyłącz moduł bezprzewodowy WiFi oraz serwer logów TCP.
// 0 = całkowite wyłączenie modułów radiowych WiFi i Bluetooth (WIFI_OFF, btStop),
//     ESP32 nie nawiązuje żadnych połączeń bezprzewodowych. Logi lecą wyłącznie
//     przez port szeregowy UART (USB) oraz do pamięci RAM/pendrive (CrashLog).
// 1 = normalna praca z połączeniem WiFi (STA) i serwerem TCP (port 12345).
#define ENABLE_WIFI 0

#if ENABLE_WIFI
#include <ESPmDNS.h>
constexpr uint16_t WIFI_LOGGER_PORT = 12345;
constexpr const char* MDNS_HOSTNAME = "unilink";
#endif

class WiFiLoggerClass : public Print {
public:
    void begin(unsigned long baud = 921600);
    void loop();

    
    // Print implementation to send to both Serial and WiFi TCP client
    virtual size_t write(uint8_t c) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;
    virtual void flush() override;

    bool isWiFiConnected() const {
#if ENABLE_WIFI
        return wifiConnected;
#else
        return false;
#endif
    }

    bool hasClient() const {
#if ENABLE_WIFI
        return clientConnected;
#else
        return false;
#endif
    }

    // Zapisz crash log na pendrive (surowe ramki + logi tekstowe).
    // reason: krotki opis powodu zrzutu (np. "RADIO TIMEOUT", "SYSTEM RESET").
    void dumpCrashLog(const char* reason = nullptr);

    // Błyskawiczna migawka w RAM (128 ramek magistrali + 8KB logów ze STAT)
    // bez blokowania pętli głównej i zapisu na dysk
    void captureCrashSnapshot(const char* reason);
    bool hasCrashSnapshot() const;
    void clearCrashSnapshot();
    static constexpr size_t BLACKBOX_SIZE = 8192;

private:
#if ENABLE_WIFI
    WiFiServer server{WIFI_LOGGER_PORT};
    WiFiClient activeClient;
#endif
    bool wifiConnected = false;
    bool clientConnected = false;
    unsigned long lastReconnectAttempt = 0;

    // Bufor kołowy (czarna skrzynka) przechowujący ostatnie ~8KB logów
    char blackboxBuf[BLACKBOX_SIZE];
    size_t blackboxHead = 0;
    size_t blackboxTail = 0;
    bool blackboxFull = false;

    void addToBlackbox(uint8_t c);
};

extern WiFiLoggerClass WiFiLogger;

#endif // WIFI_LOGGER_H
