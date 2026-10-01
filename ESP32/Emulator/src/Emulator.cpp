// =============================================================================
// Emulator.cpp — emulator zmieniarki CD Sony Unilink + odtwarzanie z pendrive'a
// =============================================================================
// Radio: Sony CDX-M670. Wyjscie audio: PCM5102A (I2S). Nosnik: pendrive USB
// (foldery CD01..CD14).
//
// Watki:
//   przerwanie zegara (IRAM, poziom 3)  — bity magistrali, ping/claim/grant
//   zadanie "unilink" (rdzen 1, prio 20) — protokol i logika zmieniarki
//                                          (UnilinkProtocol + CdChanger)
//   zadanie audio (rdzen 0)             — dekodowanie -> I2S
//   zadania USB (rdzen 0)               — stos USB Host, montowanie pendrive'a
//   loop() (rdzen 1, prio 1)            — zasilanie, skan pendrive'a, zapis NVS
//                                          w bezpiecznym oknie, crash logi
// Nic, co robi loop(), nie wplywa na czas odpowiedzi na magistrali.
// =============================================================================

#include "AudioPlayer.h"
#include "CdChanger.h"
#include "Config.h"
#include "UnilinkBus.h"
#include "UnilinkProtocol.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_system.h>

static bool          powerLatchActive = false;
static unsigned long busOffSinceMs    = 0;

static const char* resetReasonName(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        default:               return "OTHER";
    }
}

// Radio wylaczone na stale: zapis stanu i odciecie wlasnego zasilania
// (przetwornica trzymana pinem PIN_POWER_LATCH).
static void powerOff()
{
    Serial.println("=== BUS_ON=0 przez 4 s: zapis stanu i odciecie zasilania ===");
    CdChanger::persistNow();
    UnilinkProtocol::persistNow();
    if (Serial.hasCrashSnapshot()) Serial.dumpCrashLog();
    Serial.flush();
#if ENABLE_WIFI
    delay(5000);   // pakiety TCP musza fizycznie wyjsc w eter
#else
    delay(200);
#endif
    if (UnilinkBus::busOnRaw()) {   // radio wrocilo w miedzyczasie
        busOffSinceMs = 0;
        return;
    }
    digitalWrite(PIN_POWER_LATCH, LOW);
    powerLatchActive = false;
    // Gdy plytka jest dalej zasilana (USB z PC, kondensatory), czekamy na radio.
    while (!UnilinkBus::busOnRaw()) {
        delay(20);
    }
    busOffSinceMs = 0;
}

void setup()
{
    // Linie magistrali od pierwszej chwili w stanie wysokiej impedancji.
    pinMode(PIN_DATA, INPUT);
    pinMode(PIN_CLOCK, INPUT);
    pinMode(PIN_BUS_ON, INPUT);

    pinMode(PIN_POWER_LATCH, OUTPUT);
    digitalWrite(PIN_POWER_LATCH, HIGH);
    powerLatchActive = true;

    Serial.begin(921600);
    Serial.println("--- Sony Unilink: emulator zmieniarki CD + audio z USB ---");

    // Magistrala startuje zanim ruszy cokolwiek wolnego: radio robi discovery
    // ~1-2 s po wlaczeniu i zmieniarka musi juz wtedy odpowiadac.
    CdChanger::begin();
    UnilinkProtocol::begin();

    if (!LittleFS.begin(true)) {
        Serial.println("[LittleFS] UWAGA: blad inicjalizacji partycji LittleFS!");
    }
    if (audioInit()) {
        Serial.println("[Audio] Inicjalizacja zlecona (dziala w tle).");
    } else {
        Serial.println("[Audio] UWAGA: inicjalizacja I2S nie powiodla sie.");
    }

    const esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT) {
        Serial.printf("[CrashLog] Restart ESP32: %s\n", resetReasonName(rr));
        char reason[48];
        snprintf(reason, sizeof(reason), "ESP RESTART: %s", resetReasonName(rr));
        // Zrzut na pendrive nastapi po wylaczeniu radia (pendrive nie jest
        // jeszcze zamontowany).
        Serial.captureCrashSnapshot(reason);
    }
}

void loop()
{
    const unsigned long now = millis();

    if (UnilinkBus::busOnRaw()) {
        busOffSinceMs = 0;
        if (!powerLatchActive) {
            digitalWrite(PIN_POWER_LATCH, HIGH);
            powerLatchActive = true;
        }
    } else {
        if (busOffSinceMs == 0) busOffSinceMs = now ? now : 1;
        const unsigned long off = now - busOffSinceMs;
        // Radio wylaczone (dluzej niz faza BUS_ON=0 jego discovery) — teraz
        // zapis na pendrive niczego nie zakloci.
        if (off >= CRASHLOG_DUMP_AFTER_BUS_OFF_MS && Serial.hasCrashSnapshot()) {
            Serial.dumpCrashLog();
        }
        if (off >= BUS_OFF_SUICIDE_DELAY_MS) {
            powerOff();
        }
    }

    audioLoop();

    const bool flashOk = UnilinkProtocol::flashWriteWindow();
    CdChanger::servicePersist(flashOk);
    UnilinkProtocol::servicePersist(flashOk);

    WiFiLogger.loop();
    delay(2);
}
