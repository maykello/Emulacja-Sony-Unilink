#include "WiFiLogger.h"
#include "UsbDrive.h"
#include <FS.h>

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
            // Jeśli jest stary klient, zrzucamy go na rzecz nowego (np. po restarcie skryptu Pythona)
            if (activeClient && activeClient.connected()) {
                ::Serial.println("[WiFiLogger] Zrzucanie starego klienta (nowe połączenie).");
                activeClient.stop();
            }
            
            activeClient = newClient;
            activeClient.setNoDelay(true);
            clientConnected = true;
            ::Serial.printf("[WiFiLogger] Nowy klient połączenia z IP: %s\n", activeClient.remoteIP().toString().c_str());
            activeClient.printf("=== SONY UNILINK ESP32 LOG STREAM (%s.local) ===\n", MDNS_HOSTNAME);
        }

        // 3. Sprawdzanie stanu obecnego klienta i czyszczenie bufora RX
        if (clientConnected && activeClient) {
            // Skrypt w Pythonie wysyła "pingi" (puste znaki nowej linii). 
            // Musimy je na bieżąco odbierać i ignorować, w przeciwnym razie bufor RX
            // ESP32 się zapcha. Funkcja connected() potrafi zwracać true nawet po zerwaniu,
            // dopóki w buforze odbiorczym znajdują się nieodczytane bajty!
            while (activeClient.available()) {
                activeClient.read(); 
            }
            
            if (!activeClient.connected()) {
                clientConnected = false;
                activeClient.stop();
                ::Serial.println("[WiFiLogger] Klient rozłączony.");
            }
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
    addToBlackbox(c);
    ::Serial.write(c);
    if (clientConnected && activeClient.connected()) {
        activeClient.write(c);
    }
    return 1;
}

size_t WiFiLoggerClass::write(const uint8_t *buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        addToBlackbox(buffer[i]);
    }
    ::Serial.write(buffer, size);
    if (clientConnected && activeClient.connected()) {
        activeClient.write(buffer, size);
    }
    return size;
}

void WiFiLoggerClass::flush() {
    ::Serial.flush();
    if (clientConnected && activeClient.connected()) {
        activeClient.flush();
    }
}

void WiFiLoggerClass::addToBlackbox(uint8_t c) {
    blackboxBuf[blackboxHead] = (char)c;
    blackboxHead = (blackboxHead + 1) % BLACKBOX_SIZE;
    if (blackboxHead == blackboxTail) {
        blackboxTail = (blackboxTail + 1) % BLACKBOX_SIZE;
        blackboxFull = true;
    }
}

void WiFiLoggerClass::dumpBlackbox() {
    // Zapobiegaj rekursji logowania w trakcie zrzutu
    static bool dumping = false;
    if (dumping) return;
    dumping = true;
    
    if (!usbDriveIsMounted()) {
        ::Serial.println("\n[BlackBox] USB niezamontowane - brak zrzutu logow.");
        dumping = false;
        return;
    }
    
    fs::FS& fs = usbDriveGetFS();
    if (!fs.exists("/Logs")) {
        fs.mkdir("/Logs");
    }
    
    char filename[64];
    snprintf(filename, sizeof(filename), "/Logs/dump_%lu.txt", millis());
    
    ::Serial.printf("\n[BlackBox] Zrzucam bufor do %s...\n", filename);
    
    File f = fs.open(filename, FILE_WRITE);
    if (!f) {
        ::Serial.println("[BlackBox] Błąd otwarcia pliku! Upewnij się, że pendrive to FAT32 i nie jest uszkodzony.");
        dumping = false;
        return;
    }
    
    size_t count = 0;
    while (blackboxTail != blackboxHead || blackboxFull) {
        f.write((uint8_t)blackboxBuf[blackboxTail]);
        blackboxTail = (blackboxTail + 1) % BLACKBOX_SIZE;
        blackboxFull = false;
        count++;
    }
    f.close();
    
    ::Serial.printf("[BlackBox] Zapisano %d bajtów.\n", count);
    dumping = false;
}

