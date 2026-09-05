#include "WiFiLogger.h"

#ifndef WIFI_SSID
#define WIFI_SSID "Mayk3lGames2G"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

WiFiLoggerClass WiFiLogger;

void WiFiLoggerClass::begin(unsigned long baud) {
    ::Serial.begin(baud);
    ::Serial.println("\n[WiFiLogger] Inicjalizacja WiFi STA...");
    ::Serial.printf("[WiFiLogger] Łączenie z siecią: %s\n", WIFI_SSID);


    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Non-blocking wait in setup (up to 3 seconds for initial link)
    unsigned long startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 3000) {
        delay(100);
        ::Serial.print(".");
    }
    ::Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        ::Serial.printf("[WiFiLogger] Połączono! IP: %s\n", WiFi.localIP().toString().c_str());

        if (MDNS.begin(MDNS_HOSTNAME)) {
            MDNS.addService("log", "tcp", WIFI_LOGGER_PORT);
            ::Serial.printf("[WiFiLogger] mDNS uruchomiony: http://%s.local\n", MDNS_HOSTNAME);
        } else {
            ::Serial.println("[WiFiLogger] Błąd uruchomienia mDNS!");
        }

        server.begin();
        server.setNoDelay(true);
        ::Serial.printf("[WiFiLogger] Serwer TCP logów nasłuchuje na porcie %d\n", WIFI_LOGGER_PORT);
    } else {
        ::Serial.println("[WiFiLogger] Ostrzeżenie: Brak połączenia WiFi przy starcie. Łączenie w tle...");
    }
}

void WiFiLoggerClass::loop() {
    // 1. Sprawdzanie stanu WiFi
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiConnected) {
            wifiConnected = true;
            ::Serial.printf("\n[WiFiLogger] Połączono z WiFi! IP: %s\n", WiFi.localIP().toString().c_str());
            if (MDNS.begin(MDNS_HOSTNAME)) {
                MDNS.addService("log", "tcp", WIFI_LOGGER_PORT);
                ::Serial.printf("[WiFiLogger] mDNS domena: %s.local\n", MDNS_HOSTNAME);
            }
            server.begin();
            server.setNoDelay(true);
        }

        // 2. Akceptowanie nowych połączeń klienta TCP
        if (server.hasClient()) {
            WiFiClient newClient = server.accept();
            if (!activeClient || !activeClient.connected()) {
                activeClient = newClient;
                activeClient.setNoDelay(true);
                clientConnected = true;
                ::Serial.printf("[WiFiLogger] Nowy klient połączenia z IP: %s\n", activeClient.remoteIP().toString().c_str());
                activeClient.printf("=== SONY UNILINK ESP32 LOG STREAM (%s.local) ===\n", MDNS_HOSTNAME);
            } else {
                newClient.stop(); // Tylko jeden aktywny klient naraz
            }
        }

        // 3. Sprawdzanie stanu obecnego klienta
        if (clientConnected && !activeClient.connected()) {
            clientConnected = false;
            activeClient.stop();
            ::Serial.println("[WiFiLogger] Klient rozłączony.");
        }
    } else {
        if (wifiConnected) {
            wifiConnected = false;
            clientConnected = false;
            ::Serial.println("[WiFiLogger] Połączenie WiFi utracone.");
        }
    }
}


size_t WiFiLoggerClass::write(uint8_t c) {
    ::Serial.write(c);
    if (clientConnected && activeClient.connected()) {
        // Zabezpieczenie przed blokowaniem pętli przy słabym WiFi
        if (activeClient.availableForWrite() >= 1) {
            activeClient.write(c);
        }
    }
    return 1;
}

size_t WiFiLoggerClass::write(const uint8_t *buffer, size_t size) {
    ::Serial.write(buffer, size);
    if (clientConnected && activeClient.connected()) {
        // Sprawdzamy czy bufor TCP w ESP32 pomieści loga. 
        // Jeśli nie - UTRACAMY TEN LOG przez WiFi, ale NIE BLOKUJEMY 
        // pętli głównej (co by spowodowało crash emulacji UniLink).
        if (activeClient.availableForWrite() >= size) {
            activeClient.write(buffer, size);
        }
    }
    return size;
}

