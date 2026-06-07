// =============================================================================
// Emulator.ino — emulator zmieniarki CD Sony UniLink (10-CD) + audio z USB
// =============================================================================
// Obsluga radia: MEX-BT3800u + CDX-M670. Wyjscie audio: PCM5102A (I2S).
// Nosnik: pendrive USB (foldery CD01..CD10).
//
// Architektura (warstwy):
//   Config.h          — piny, ustawienia sprzetowe, stale czasowe
//   UnilinkBus        — warstwa fizyczna: bit-banging ISR, ramki, Slave Break
//   CdChanger         — model zmieniarki: stan, czas, NVS, nawigacja, audio
//   UnilinkProtocol   — interpretacja ramek, discovery, kwirki CDX-M670
//   AudioPlayer       — dekodowanie i odtwarzanie plikow (I2S/PCM5102A)
//   UsbDrive          — USB Host MSC (FAT32)
//   Emulator.ino      — spiecie warstw (setup + loop)
// =============================================================================

#include <Arduino.h>
#include "Config.h"
#include "UnilinkBus.h"
#include "CdChanger.h"
#include "UnilinkProtocol.h"
#include "AudioPlayer.h"

// Stan zasilania magistrali (do wykrywania zboczy BUS_ON).
static bool busPoweredLast = false;

void setup() {
    Serial.begin(115200);
    Serial.println("--- Sony UniLink EMULATOR (10-CD) v9 + AUDIO ---");
    Serial.println("Obsluga: MEX-BT3800u + CDX-M670 + PCM5102A DAC + USB");
    Serial.println("Oczekuje na radio (Stan C0 - Init)...");

    // Pamiec nieulotna — wczytaj ostatnio odtwarzany utwor.
    CdChanger::begin();

    // Audio (USB Host + I2S). MUSI byc PRZED UnilinkBus::begin(), bo uruchomienie
    // USB Host uzywa delay(), a magistrala nie ma jeszcze podlaczonego przerwania.
    if (audioInit()) {
        Serial.println("[Audio] Gotowy do odtwarzania.");
    } else {
        Serial.println("[Audio] UWAGA: Brak nosnika USB — emulator dziala bez dzwieku.");
    }

    // Warstwa fizyczna magistrali (piny + przerwanie zegara).
    UnilinkBus::begin();

    // Stan sesji protokolu.
    UnilinkProtocol::begin();
}

void loop() {
    unsigned long now = millis();

    // ===== Detekcja zasilania magistrali (BUS_ON) =====
    // Prawdziwa zmieniarka jest zasilana z magistrali: gdy BUS=0, jest WYLACZONA
    // i nie odpowiada. CDX-M670 wykorzystuje fazy BUS=0/1 do dyskryminacji
    // urzadzen wewnetrznych od zewnetrznych — odpowiedz przy BUS=0 = petla RESET.
    bool busPowered = (digitalRead(PIN_BUS_ON) == HIGH);
    if (busPowered != busPoweredLast) {
        busPoweredLast = busPowered;
        Serial.printf("=== BUS_ON = %d ===\n", busPowered ? 1 : 0);
        if (!busPowered) {
            Serial.println("=== SEN: magistrala wylaczona — zatrzymuje audio ===");
            CdChanger::sleep();
            UnilinkProtocol::onBusOff();
            UnilinkBus::resetRx();  // bajty z fazy BUS=0 sa "obce"
        } else {
            CdChanger::wake();
        }
    }

    // ===== Wykrycie nosnika USB (wznowienie zapamietanej plyty) =====
    CdChanger::serviceMediaMount();

    // ===== Timeout: radio zniknelo =====
    if (UnilinkProtocol::serviceTimeout(now)) {
        CdChanger::sleep();
        Serial.println("--- Radio timeout (5s). Reset do C0 + STOP audio ---");
    }

    // ===== Maszyna stanow + sekundnik =====
    CdChanger::update(now, UnilinkProtocol::isAllocated());

    // ===== Auto-next po koncu utworu =====
    CdChanger::serviceAutoAdvance();

    // ===== Slave Break (zgloszenie chec aktualizacji wyswietlacza) =====
    UnilinkProtocol::serviceSlaveBreak(busPowered);

    // ===== Odbior i przetwarzanie ramki =====
    // Bufor oproznia sie zawsze (po READ_SILENCE_US ciszy), ale przetwarzamy
    // tylko gdy BUS=1. Przy BUS=0 bajty sa pochlaniane bez odpowiedzi.
    static uint8_t packet[RX_BUFFER_SIZE];
    int count = UnilinkBus::readPacketIfIdle(packet, sizeof(packet), READ_SILENCE_US);
    if (count > 0 && busPowered) {
        if (DEBUG_VERBOSE) {
            Serial.print("RX: ");
            for (int i = 0; i < count; i++) {
                if (packet[i] < 0x10) Serial.print("0");
                Serial.print(packet[i], HEX);
                Serial.print(" ");
            }
            Serial.println();
        }

        UnilinkProtocol::handlePacket(packet, count);
    }

    // ===== Dekodowanie audio (hot-plug + wykrywanie konca utworu) =====
    audioLoop();
}
