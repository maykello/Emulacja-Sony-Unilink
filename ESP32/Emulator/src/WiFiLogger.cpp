#include "WiFiLogger.h"
#include "UsbDrive.h"
#include <FS.h>

// UWAGA: NIE WOLNO includowac Config.h w tym pliku!
// Config.h zawiera `#define Serial WiFiLogger`, co powodowaloby nieskonczona
// rekursje w write() -> ::Serial.write() -> WiFiLogger::write() -> stack overflow.
// Stale z Config.h uzywane w tym pliku kopiujemy recznie ponizej:
static constexpr int CRASHLOG_MAX_FILES_LOCAL = 10;  // musi byc zgodne z CRASHLOG_MAX_FILES w Config.h

#ifndef WIFI_SSID
#define WIFI_SSID "Mayk3lGames2G"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

WiFiLoggerClass WiFiLogger;

// Logi pisze kilka zadan naraz (magistrala, audio, USB, petla glowna).
static portMUX_TYPE s_blackboxMux = portMUX_INITIALIZER_UNLOCKED;

void WiFiLoggerClass::begin(unsigned long baud) {
#if !ARDUINO_USB_CDC_ON_BOOT
    // Bez bufora nadawczego kazdy printf czeka, az bajty zejda do 128-bajtowego
    // FIFO UART — zadanie magistrali nie moze na tym stac.
    ::Serial.setTxBufferSize(8192);
#endif
    ::Serial.begin(baud);

#if ENABLE_WIFI
    ::Serial.println("\n[WiFiLogger] Inicjalizacja WiFi STA...");
    ::Serial.printf("[WiFiLogger] Łączenie z siecią: %s\n", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    // Bez czekania na polaczenie — loop() uruchomi serwer, gdy WiFi wstanie.

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
#else
    // Całkowite wyłączenie modułów radiowych WiFi oraz Bluetooth (tryb offline).
    // Bez zadnego czekania: radio robi discovery ~1-2 s po wlaczeniu i
    // zmieniarka musi juz wtedy sluchac magistrali.
    WiFi.mode(WIFI_OFF);
    btStop();
    wifiConnected = false;
    clientConnected = false;
    ::Serial.println("\n[WiFiLogger] Moduły radiowe WiFi oraz Bluetooth: WYŁĄCZONE (tryb offline).");
#endif
}

void WiFiLoggerClass::loop() {
#if ENABLE_WIFI
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
#endif
}


size_t WiFiLoggerClass::write(uint8_t c) {
    portENTER_CRITICAL(&s_blackboxMux);
    addToBlackbox(c);
    portEXIT_CRITICAL(&s_blackboxMux);
    ::Serial.write(c);
#if ENABLE_WIFI
    if (clientConnected && activeClient.connected()) {
        activeClient.write(c);
    }
#endif
    return 1;
}

size_t WiFiLoggerClass::write(const uint8_t *buffer, size_t size) {
    portENTER_CRITICAL(&s_blackboxMux);
    for (size_t i = 0; i < size; i++) {
        addToBlackbox(buffer[i]);
    }
    portEXIT_CRITICAL(&s_blackboxMux);
    ::Serial.write(buffer, size);
#if ENABLE_WIFI
    if (clientConnected && activeClient.connected()) {
        activeClient.write(buffer, size);
    }
#endif
    return size;
}

void WiFiLoggerClass::flush() {
    ::Serial.flush();
#if ENABLE_WIFI
    if (clientConnected && activeClient.connected()) {
        activeClient.flush();
    }
#endif
}

void WiFiLoggerClass::addToBlackbox(uint8_t c) {
    blackboxBuf[blackboxHead] = (char)c;
    blackboxHead = (blackboxHead + 1) % BLACKBOX_SIZE;
    if (blackboxHead == blackboxTail) {
        blackboxTail = (blackboxTail + 1) % BLACKBOX_SIZE;
        blackboxFull = true;
    }
}

#include "Diagnostics.h"
#include <esp_system.h>
#include <dirent.h>

// Nazwa powodu resetu ESP32 — do nagłówka crash logu
static const char* espResetReasonName() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "POWERON";
        case ESP_RST_SW:       return "SW_RESET";
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "UNKNOWN";
    }
}

// Znajdz najwyzszy numer crash_NNNNN.txt w /CrashLogs/
// i zwroc go. Jesli folder pusty, zwraca 0.
// Jednoczesnie liczy ile plikow jest w folderze (outCount).
static int findMaxCrashNumber(fs::FS& fs, int& outCount) {
    outCount = 0;
    int maxNum = 0;

    File dir = fs.open("/CrashLogs");
    if (!dir || !dir.isDirectory()) return 0;

    File entry = dir.openNextFile();
    while (entry) {
        const char* name = entry.name();
        // Szukamy plików crash_NNNNN.txt
        if (strncmp(name, "crash_", 6) == 0) {
            int num = atoi(name + 6);
            if (num > maxNum) maxNum = num;
            outCount++;
        }
        entry = dir.openNextFile();
    }
    return maxNum;
}

// Usun najstarszy (najnizszy numer) plik crash_*.txt z /CrashLogs/
static void deleteOldestCrashLog(fs::FS& fs) {
    int minNum = 999999;
    char minName[64] = {};

    File dir = fs.open("/CrashLogs");
    if (!dir || !dir.isDirectory()) return;

    File entry = dir.openNextFile();
    while (entry) {
        const char* name = entry.name();
        if (strncmp(name, "crash_", 6) == 0) {
            int num = atoi(name + 6);
            if (num < minNum) {
                minNum = num;
                snprintf(minName, sizeof(minName), "/CrashLogs/%s", name);
            }
        }
        entry = dir.openNextFile();
    }

    if (minName[0]) {
        fs.remove(minName);
        ::Serial.printf("[CrashLog] Rotacja: usunięto %s\n", minName);
    }
}

// --- MIGAWEK RAM DLA ODROCZONEGO ZRZUTU ---
static char snapshotTextBuf[WiFiLoggerClass::BLACKBOX_SIZE];
static size_t snapshotHead = 0;
static size_t snapshotTail = 0;
static bool snapshotFull = false;
static unsigned long snapshotUptime = 0;
static char snapshotReason[64] = "";
static bool s_hasCrashSnapshot = false;

void WiFiLoggerClass::captureCrashSnapshot(const char* reason) {
    if (s_hasCrashSnapshot) return; // zachowaj pierwsze zdarzenie z danej trasy
    snapshotUptime = millis();
    if (reason) {
        strncpy(snapshotReason, reason, sizeof(snapshotReason) - 1);
        snapshotReason[sizeof(snapshotReason) - 1] = '\0';
    } else {
        strncpy(snapshotReason, "UNKNOWN", sizeof(snapshotReason) - 1);
    }
    portENTER_CRITICAL(&s_blackboxMux);
    snapshotHead = blackboxHead;
    snapshotTail = blackboxTail;
    snapshotFull = blackboxFull;
    portEXIT_CRITICAL(&s_blackboxMux);
    // Porcjami: dluga sekcja krytyczna opoznilaby przerwanie zegara magistrali
    // (ten sam rdzen) i przeklamala bity.
    constexpr size_t CHUNK = 256;
    for (size_t off = 0; off < sizeof(blackboxBuf); off += CHUNK) {
        const size_t n = (sizeof(blackboxBuf) - off < CHUNK) ? sizeof(blackboxBuf) - off : CHUNK;
        portENTER_CRITICAL(&s_blackboxMux);
        memcpy(snapshotTextBuf + off, blackboxBuf + off, n);
        portEXIT_CRITICAL(&s_blackboxMux);
    }

    Diagnostics::captureSnapshot();
    s_hasCrashSnapshot = true;
}

bool WiFiLoggerClass::hasCrashSnapshot() const {
    return s_hasCrashSnapshot;
}

void WiFiLoggerClass::clearCrashSnapshot() {
    s_hasCrashSnapshot = false;
    Diagnostics::clearSnapshot();
}

void WiFiLoggerClass::dumpCrashLog(const char* reason) {
    // Zapobiegaj rekursji logowania w trakcie zrzutu
    static bool dumping = false;
    if (dumping) return;
    dumping = true;

    if (!usbDriveIsMounted()) {
        ::Serial.println("\n[CrashLog] USB niezamontowane - brak zrzutu.");
        dumping = false;
        return;
    }

    const bool fromSnapshot = s_hasCrashSnapshot;
    const char* effReason = fromSnapshot ? snapshotReason : (reason ? reason : "UNKNOWN");
    const unsigned long effUptime = fromSnapshot ? snapshotUptime : millis();

    fs::FS& fs = usbDriveGetFS();
    if (!fs.exists("/CrashLogs")) {
        fs.mkdir("/CrashLogs");
    }

    // Rotacja: usun najstarszy plik jesli >= CRASHLOG_MAX_FILES
    int fileCount = 0;
    int maxNum = findMaxCrashNumber(fs, fileCount);
    while (fileCount >= CRASHLOG_MAX_FILES_LOCAL) {
        deleteOldestCrashLog(fs);
        fileCount--;
    }

    // Sekwencyjny numer pliku
    int nextNum = maxNum + 1;
    char filename[64];
    snprintf(filename, sizeof(filename), "/CrashLogs/crash_%05d.txt", nextNum);

    ::Serial.printf("\n[CrashLog] Zapisuję crash log: %s (reason: %s%s)\n",
                    filename, effReason, fromSnapshot ? " [ODROCZONY Z RAM]" : "");

    File f = fs.open(filename, FILE_WRITE);
    if (!f) {
        ::Serial.println("[CrashLog] Błąd otwarcia pliku! Pendrive FAT32?");
        dumping = false;
        return;
    }

    // === NAGŁÓWEK ===
    f.println("===== CRASH LOG =====");
    f.printf("Reason:    %s\n", effReason);
    f.printf("Uptime:    %lu ms\n", effUptime);
    f.printf("ESP Reset: %s\n", espResetReasonName());
    f.printf("File:      %s\n", filename);
    f.println("=====================");
    f.println();

    // === SUROWE RAMKI MAGISTRALI ===
    if (fromSnapshot) {
        Diagnostics::dumpSnapshotToFile(f);
    } else {
        Diagnostics::dumpToFile(f);
    }
    f.println();

    // === LOGI TEKSTOWE (bufor kołowy) ===
    f.printf("===== TEXT LOG (last ~%d bytes) =====\n", BLACKBOX_SIZE);
    size_t count = 0;
    const char* srcBuf = fromSnapshot ? snapshotTextBuf : blackboxBuf;
    size_t pos = fromSnapshot ? (snapshotFull ? snapshotHead : snapshotTail)
                              : (blackboxFull ? blackboxHead : blackboxTail);
    size_t end = fromSnapshot ? snapshotHead : blackboxHead;
    bool hasData = fromSnapshot ? (snapshotFull || (snapshotTail != snapshotHead))
                                : (blackboxFull || (blackboxTail != blackboxHead));

    if (hasData) {
        do {
            f.write((uint8_t)srcBuf[pos]);
            pos = (pos + 1) % BLACKBOX_SIZE;
            count++;
        } while (pos != end);
    }
    f.println("\n=====================================");

    f.close();

    ::Serial.printf("[CrashLog] Zapisano %s (%d B logów)\n", filename, count);

    if (fromSnapshot) {
        s_hasCrashSnapshot = false;
        Diagnostics::clearSnapshot();
    }
    dumping = false;
}
