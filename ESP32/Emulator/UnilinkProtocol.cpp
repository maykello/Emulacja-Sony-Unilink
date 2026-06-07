// =============================================================================
// UnilinkProtocol.cpp — implementacja warstwy aplikacyjnej protokolu UniLink
// =============================================================================
#include "UnilinkProtocol.h"
#include "Config.h"
#include "UnilinkBus.h"
#include "CdChanger.h"
#include "Diagnostics.h"

namespace UnilinkProtocol {

// --- STAN SESJI ---
static bool          deviceAllocated = false;
static unsigned long lastPingTime    = 0;

// Adres przydzielony przez radio. KLUCZOWE: radio NIE zawsze przydziela 0x31 —
// po resecie potrafi nadac kolejny wolny adres (0x31..0x3A). Adoptujemy KAZDY
// nadany adres i uzywamy go we wszystkich odpowiedziach.
static uint8_t myAddr = ADDR_DEFAULT;

// --- DETEKCJA / KWIRKI CDX-M670 ---
// CDX-M670 ma 2-fazowe discovery. Faza preliminary (markery 3B/DB) jest dla
// wewnetrznych urzadzen radia; prawdziwa zmieniarka odpowiada dopiero w fazie
// glownej. Odpowiedz w fazie preliminary => zly slot => nieskonczona petla RESET.
static bool          isCdxM670         = false;
static unsigned long lastPreliminaryTime = 0;
static int           anyoneIgnoredCount  = 0;   // tylko do logowania
static int           resetLoopCount      = 0;   // licznik resetow w petli

// --- SLAVE BREAK / OCHRONA KOLIZJI ---
static unsigned long suppressBreakUntil = 0;
static unsigned long lastBreakTime      = 0;
// Czas ostatniego zadania ekranu (01 13) OD MASTERA — gdy radio aktywnie
// pollu­je wyswietlacz, Slave Break jest zbedny i grozi kolizja.
static unsigned long lastDisplayRequestTime = 0;

// ============================================================
// API
// ============================================================
void begin() {
    lastPingTime = millis();
}

bool isAllocated() {
    return deviceAllocated;
}

// ------------------------------------------------------------
// Pelny reset stanu sesji (wspolny rdzen dla BUS-off).
// ------------------------------------------------------------
void onBusOff() {
    deviceAllocated = false;
    myAddr = ADDR_DEFAULT;
    // Reset markerow preliminary (ale NIE isCdxM670 — to zostaje po wykryciu,
    // zeby przezyc cykl BUS_ON).
    lastPreliminaryTime = 0;
    anyoneIgnoredCount = 0;
    resetLoopCount = 0;
    lastDisplayRequestTime = 0;
}

bool serviceTimeout(unsigned long now) {
    if (deviceAllocated && (now - lastPingTime > RADIO_TIMEOUT_MS)) {
        deviceAllocated = false;
        myAddr = ADDR_DEFAULT;
        return true;
    }
    return false;
}

void serviceSlaveBreak(bool busPowered) {
    // SLAVE BREAK potrzebny: aktualizacja WYSWIETLACZA jest inicjowana przez
    // slave'a. Master sam NIGDY nie pyta o ekran — dopiero gdy wykryje nasz
    // break, wysyla 01 15 ("kto chce ekran?") i 01 13 ("dawaj dane").
    // Robimy to bezpiecznie: tylko po >BREAK_SILENCE_US ciszy (po ACK, w realnym
    // idle), z natychmiastowym porzuceniem gdy radio ruszy z zegarem.
    bool silToBreak = (UnilinkBus::microsSinceLastClock() > BREAK_SILENCE_US);
    bool busy = UnilinkBus::isTransmitting();
    bool stateAllowsBreak = (CdChanger::state() == CdChanger::STATE_PLAYING);

    // Gdy radio aktywnie pollu­je nasz wyswietlacz (dostalismy 01 13 niedawno),
    // Slave Break jest zbedny i grozi kolizja z 0x3B/0x14 -> SYSTEM RESET.
    bool radioPollingDisplay =
        (lastDisplayRequestTime != 0 &&
         (millis() - lastDisplayRequestTime) < DISPLAY_POLL_ACTIVE_MS);

    if (CdChanger::isDisplayDirty() && silToBreak && !busy && deviceAllocated &&
        stateAllowsBreak && busPowered && !radioPollingDisplay &&
        millis() >= suppressBreakUntil) {
        if (millis() - lastBreakTime > BREAK_INTERVAL_MS) {
            UnilinkBus::issueSlaveBreak();
            lastBreakTime = millis();
            // NIE kasujemy display-dirty — skasuje sie gdy radio odpyta 01 13.
        }
    }
}

// ------------------------------------------------------------
// Buduje i wysyla 16-bajtowy status CD (odpowiedz na 01 13).
// ------------------------------------------------------------
static void sendDisplayStatus() {
    // PAKIET ZGODNY Z PRAWDZIWA ZMIENIARKA (CDX-M670 sniff):
    //   70 <addr> C0 00 | P1 | 00 00 00 00 30 <TRK> <MIN> <SEK> <DISC> | P2 | 00
    // gdzie (sniff: ...30 F1 F0 00 88 = TR1 0:00, ...30 F3 F0 00 88 = TR3):
    //   D5 = 0x30 (marker stalej)
    //   D6 = TRACK BCD (F-padding: F1=01..F9=09, potem 10..99)
    //   D7 = MINUTY BCD (F-padding: F0=0)
    //   D8 = SEKUNDY BCD (00..59)
    //   D9 = NUMER PLYTY w starszym nibblu (CD1->0x10 ... CD10->0xA0)
    uint8_t trk = CdChanger::track();
    uint8_t min = CdChanger::minutes();
    uint8_t sec = CdChanger::seconds();
    uint8_t dsk = CdChanger::disk();

    uint8_t secBCD = ((sec / 10) << 4) | (sec % 10);
    uint8_t trackBCD = (trk < 10) ? (0xF0 | trk)
                                  : (((trk / 10) << 4) | (trk % 10));
    uint8_t minBCD = (min < 10) ? (0xF0 | min)
                                : (((min / 10) << 4) | (min % 10));
    uint8_t discByte = (uint8_t)((dsk & 0x0F) << 4);

    UnilinkBus::sendLong(0x70, myAddr, 0xC0, 0x00,
                         0x00, 0x00, 0x00, 0x00,
                         0x30, trackBCD, minBCD, secBCD, discByte);

    if (DEBUG_VERBOSE) {
        Serial.printf(">> DISPLAY (C0): CD%d TR%d %02d:%02d (D9=0x%02X, state=0x%02X)\n",
                      dsk, trk, min, sec, discByte, CdChanger::state());
    }
}

// ============================================================
// Glowny dyspozytor ramek
// ============================================================
void handlePacket(const uint8_t* buf, int len) {
    if (len < 6) return;

    // Czarna skrzynka: rejestrujemy KAZDA ramke (tez przeklamana) — to ona
    // bywa dowodem kolizji prowadzacej do SYSTEM RESET.
    Diagnostics::recordFrame("RX", buf, len);

    lastPingTime = millis();

    uint8_t rad = buf[0];
    uint8_t tad = buf[1];
    uint8_t op1 = buf[2];
    uint8_t op2 = buf[3];

    // ===== Walidacja sumy kontrolnej naglowka =====
    // W poprawnych ramkach buf[4] = (RAD+TAD+OP1+OP2) & 0xFF. Przeklamane/
    // kolidujace ramki tego nie spelniaja — prawdziwa zmieniarka je ignoruje.
    if (buf[4] != (uint8_t)(rad + tad + op1 + op2)) {
        return;
    }

    // ===== Okno ochronne: poll mastera do INNEGO urzadzenia =====
    // Radio odpytalo swoje wewnetrzne urzadzenie (0x3B/0x71/...), ktore za chwile
    // odpowie. Wstrzymujemy Slave Break, by nie zderzyc sie z ta odpowiedzia.
    if (tad == ADDR_MASTER && rad != myAddr && rad != ADDR_BROADCAST) {
        suppressBreakUntil = millis() + FOREIGN_POLL_GUARD_MS;
    }

    // ===== Detekcja CDX-M670 + okno preliminary =====
    // 3B 10 02 11 = appoint wewnetrznego CD radia (TYLKO CDX-M670).
    // DB 10 02 12 = appoint wewnetrznego pomocniczego (TYLKO CDX-M670).
    // Po tych pakietach ignorujemy ANYONE? przez PRELIMINARY_WINDOW_MS.
    if (rad == 0x3B && tad == ADDR_MASTER && op1 == 0x02 && op2 == 0x11) {
        if (!isCdxM670) {
            isCdxM670 = true;
            Serial.println("== Wykryto CDX-M670 (widziano 3B 10 02 11) ==");
        }
        lastPreliminaryTime = millis();
        return;
    }
    if (rad == 0xDB && tad == ADDR_MASTER && op1 == 0x02 && op2 == 0x12) {
        lastPreliminaryTime = millis();
        return;
    }

    // ===== BUS AUDIO IN — diagnostyka routingu dzwieku (18 10 87 ...) =====
    // Gorny nibble op2 to licznik sekwencji, bit0 = stan. Prawdziwa zmieniarka
    // NIGDY nie wycisza wlasnego wyjscia na tej podstawie — gra caly czas.
    // Dlatego TYLKO logujemy i NIE ruszamy glosnosci DAC.
    if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x87) {
        bool on = (op2 & 0x01) != 0;
        Serial.printf(">> [Audio Bus] Radio sygnalizuje audio %s (87 %02X) — tylko log\n",
                      on ? "ON" : "OFF", op2);
        return;
    }

    // ===== 1. ANYONE? (18 10 01 02) — broadcast discovery =====
    // Odpowiadamy TYLKO gdy nie mamy jeszcze adresu (inaczej radio przydzieli
    // nam drugi/trzeci adres myslac, ze jestesmy nowym urzadzeniem).
    if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x02) {
        if (!deviceAllocated) {
            // CDX-M670: ignoruj ANYONE? w oknie preliminary — to faza dla
            // wewnetrznych urzadzen radia, nie dla nas.
            if (isCdxM670 && lastPreliminaryTime != 0 &&
                (millis() - lastPreliminaryTime) < PRELIMINARY_WINDOW_MS) {
                anyoneIgnoredCount++;
                Serial.printf(">> [CDX-M670] Ignoruje ANYONE? w oknie preliminary (#%d, %lums po DB)\n",
                              anyoneIgnoredCount, millis() - lastPreliminaryTime);
                return;
            }

            // ATRYBUTY ZGODNE Z PRAWDZIWA ZMIENIARKA (CDX-M670 sniff):
            //   10 30 8C D0 | 9C | 05 A8 1F A3 | 0B 00
            const uint8_t attr[] = {0x10, 0x30, 0x8C, 0xD0, 0x9C, 0x05, 0xA8, 0x1F, 0xA3, 0x0B, 0x00};
            UnilinkBus::sendRaw(attr, sizeof(attr));
            Serial.println(">> Odpowiadam na ANYONE? z atrybutami");
        }
    }

    // ===== 1a. UNAPPOINTED CHANGER QUERY (18 10 01 11 / 18 10 01 01) =====
    // Prawdziwa zmieniarka odpowiada magicznym pakietem 10 18 04 00 2C 00, ktory
    // wywoluje u radia SYSTEM RESET i cykl BUS_ON inicjujacy wlasciwe discovery.
    // W trybie CDX-M670 odpowiadamy tez na op2=0x01.
    else if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x01 &&
             (op2 == 0x11 || (isCdxM670 && op2 == 0x01))) {
        if (!deviceAllocated) {
            const uint8_t magic[] = {0x10, 0x18, 0x04, 0x00, 0x2C, 0x00};
            UnilinkBus::sendRaw(magic, sizeof(magic));
            Serial.printf(">> Odpowiadam na 01 %02X (nieprzydzielona zmieniarka): magic 10 18 04 00\n", op2);
        }
    }

    // ===== 1b. SYSTEM RESET (18 10 01 00) =====
    // Radio przerywa sesje i zaczyna discovery od nowa. Zapominamy adres.
    else if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x00) {
        // Zrzuc czarna skrzynke ZANIM zresetujemy stan — pokaze ramki, ktore
        // doprowadzily do resetu radia.
        Diagnostics::dump("RADIO SYSTEM RESET 18 10 01 00");
        resetLoopCount++;
        if (deviceAllocated) {
            Serial.printf(">> Radio system reset (#%d)! Reset deviceAllocated.\n", resetLoopCount);
            deviceAllocated = false;
            CdChanger::resetToInit();
            CdChanger::clearDisplayDirty();
        } else {
            Serial.printf(">> Radio system reset (#%d) (juz bylem nieprzydzielony)\n", resetLoopCount);
        }
        myAddr = ADDR_DEFAULT;  // nastepny przydzial moze byc inny
        lastPreliminaryTime = 0;
        anyoneIgnoredCount = 0;
    }

    // ===== 2. ADDRESS APPOINT (3X 10 02 XX) =====
    // Radio przydziela adres z bloku zmieniarek (0x31..0x3A). op2 i sam adres
    // roznia sie miedzy radiami — adoptujemy KAZDE.
    else if (rad >= 0x31 && rad <= 0x3A && tad == ADDR_MASTER && op1 == 0x02) {
        myAddr = rad;
        deviceAllocated = true;
        CdChanger::resetToInit();
        // ATRYBUTY ZGODNE Z PRAWDZIWA ZMIENIARKA (sniff, adres 0x31):
        //   10 31 8C D0 | 9D | 04 A8 1F A3 | 0B 00
        // Bajty 5/6 skaluja sie z adresem tak, by suma byla stala (0xA1).
        uint8_t b5 = (uint8_t)(0x6C + myAddr);
        uint8_t b6 = (uint8_t)(0xA1 - b5);
        const uint8_t status[] = {0x10, myAddr, 0x8C, 0xD0, b5, b6, 0xA8, 0x1F, 0xA3, 0x0B, 0x00};
        UnilinkBus::sendRaw(status, sizeof(status));
        Serial.printf(">> Adres przydzielony: 0x%02X (op2=0x%02X)! deviceAllocated=true\n", myAddr, op2);
    }

    // ===== 3. SLAVE POLL — kto chce wyswietlacz? (18 10 01 15) =====
    // Format: 10 18 82 <typ> | PAR1 | 00 00 00 00 | PAR2 00, gdzie typ ekranu:
    //   0x01 = startowy/idle, 0x04 = odtwarzanie, 0x05 = przejsciowy.
    else if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x15) {
        uint8_t displayType;
        CdChanger::State st = CdChanger::state();
        if (st == CdChanger::STATE_PLAYING) {
            displayType = 0x04;  // Playing — pelny ekran z track/czas
        } else if (st == CdChanger::STATE_LOADING || st == CdChanger::STATE_SEEKING) {
            displayType = 0x05;  // Loading/Seeking — ekran przejsciowy
        } else {
            displayType = 0x01;  // Init/Idle — ekran startowy
        }
        UnilinkBus::sendMedium(0x10, 0x18, 0x82, displayType, 0x00, 0x00, 0x00, 0x00);
        if (DEBUG_VERBOSE) {
            Serial.printf(">> Odpowiadam na 01 15 (Slave Poll): chce ekran typ 0x%02X (stan=0x%02X)\n",
                          displayType, st);
        }
    }

    // ===== 4. PING — status query (01 12) od mastera (0x10) LUB procesora ekranu (0x14) =====
    // KLUCZOWE: CDX-M670 okresowo odpytuje status RÓWNIEŻ z procesora ekranu
    // (tad=0x14): "31 14 01 12". Brak odpowiedzi na nie powodowal, ze radio
    // uznawalo zmieniarke za niesprawna i robilo SYSTEM RESET — potwierdzone w
    // DWOCH niezaleznych zrzutach czarnej skrzynki ('31 14 01 12' tuz przed
    // kazdym resetem). Odpowiadamy temu, kto pyta (odbiorca odpowiedzi = tad).
    else if (rad == myAddr && (tad == ADDR_MASTER || tad == ADDR_DISPLAY) &&
             op1 == 0x01 && op2 == 0x12) {
        if (tad == ADDR_MASTER) CdChanger::noteFirstPing();
        // Odpowiedz: <pytajacy> <addr> 00 <state> <parity> 00
        UnilinkBus::sendShort(tad, myAddr, 0x00, CdChanger::state());
        if (DEBUG_VERBOSE) {
            Serial.printf(">> PING odpowiedz (do 0x%02X): stan=0x%02X\n", tad, CdChanger::state());
        }
    }

    // ===== 5. UPDATE DISPLAY (3X 10 01 13) =====
    // TYLKO gdy tad == 0x10 (pytanie do nas). NIE odpowiadamy na 31 14 01 13 —
    // to pytanie do display processora (0x14), nie do nas.
    else if (rad == myAddr && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x13) {
        lastDisplayRequestTime = millis();  // radio aktywnie pollu­je ekran
        CdChanger::clearDisplayDirty();
        sendDisplayStatus();
    }

    // ===== 6. WAKE UP / PLAY (3X 10 20 00) =====
    else if (rad == myAddr && tad == ADDR_MASTER && op1 == 0x20 && op2 == 0x00) {
        CdChanger::handlePlayCommand();
    }

    // ===== 7. Komendy z panelu (3X 11 XX XX) =====
    else if (rad == myAddr && tad == 0x11) {
        if (op1 == 0x26 && op2 == 0x10)      CdChanger::nextTrack();
        else if (op1 == 0x27 && op2 == 0x10) CdChanger::prevTrack();
        else if (op1 == 0x28)                CdChanger::nextDisc();
        else if (op1 == 0x29)                CdChanger::prevDisc();
    }
}

} // namespace UnilinkProtocol
