// =============================================================================
// UnilinkProtocol.cpp — implementacja warstwy aplikacyjnej protokolu UniLink
// =============================================================================
#include "UnilinkProtocol.h"
#include "Config.h"
#include "UnilinkBus.h"
#include "CdChanger.h"
#include "Diagnostics.h"
#include "UnilinkFrame.h"
#include "AddressManager.h"
#include "TxQueue.h"
#include "CdText.h"
#include "Magazine.h"
#include "AudioPlayer.h"

namespace UnilinkProtocol {

// --- STAN SESJI (AddressManager / Menedzer_Adresow) ---
// Reguly adresowania (start/Anyone/Appoint/Bus reset) zyja w czystym module
// AddressManager (host-testable, bez Arduino.h). Tutaj trzymamy jedynie biezacy
// stan i przepuszczamy zdarzenia przez AddressManager::apply. `myAddr`/
// `deviceAllocated` to projekcja AddressManager::State na nazwy uzywane w
// dyspozytorze. myAddr jest TAD-em we WSZYSTKICH odpowiedziach adresowanych do
// nas. Adoptujemy DOWOLNE ID z grupy CD (0x30..0x3F), nie tylko 0x31..0x3A.
static uint8_t myAddr          = AddressManager::ADDR_GROUP_CD;  // = ADDR_DEFAULT (0x30)
static bool    deviceAllocated = false;
static unsigned long lastPingTime = 0;

// Projekcja lokalnych zmiennych <-> AddressManager::State (jedno zrodlo prawdy
// dla regul; tutaj tylko przeladunek pol).
static inline AddressManager::State addrState() {
    return AddressManager::State{ myAddr, deviceAllocated };
}
static inline void setAddrState(const AddressManager::State& s) {
    myAddr          = s.myAddr;
    deviceAllocated = s.allocated;
}

// --- UTILITY: kod statusu z MechState (Kompendium §7.1, Wymaganie 5) ---
// Mapowanie stanu mechanizmu na bajt statusu zyje w czystym module UnilinkFrame.
// Tutaj tylko wygoda dla dyspozytora (bez koniecznosci include CdChanger.h).
static inline uint8_t statusByteFromState(CdChanger::MechState s) {
    return UnilinkFrame::statusByte(s);
}

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
// Czas ostatniego pobrania naszego ekranu przez radio (01 13). Keepalive-break
// budzi radio, gdy ta wartosc sie zestarzeje.
static unsigned long lastDisplayServedMs = 0;

// --- HARMONOGRAM RAMKI POZYCJI 0x90 (1 Hz) ---
// Znacznik millis() ostatniej aktualizacji pozycji (0x90). Co sekundę,
// w stanie Playing, zwiększamy licznik i enqueue'ujemy nową ramkę 0x90.
static unsigned long lastPositionUpdateMs = 0;

// --- KOLEJKA TX (request-poll 0x13) ---
// Zmieniarka kolejkuje blok odpowiedzi (status, disc ID, info magazynka, nazwy
// CD-TEXT, ramka czasu) i wysyla po jednej ramce na kazdy grant `0x01 0x13`
// (Kompendium §7.2 / §12.4, design.md "Cykl pracy i kolejka TX"). Instancja
// statyczna w przestrzeni nazw — caly bufor zyje w obiekcie (bez alokacji
// dynamicznej). PONG na time-poll `0x01 0x12` idzie POZA kolejka (natychmiast).
static Tx::TxQueue txQueue;

// --- DIAGNOSTYKA (liczniki resetowane co ~2s w serviceStats) ---
static unsigned long lastStatMs = 0;
static uint16_t statPoll15 = 0;   // 18 10 01 15 — slave poll (kto chce ekran)
static uint16_t statDisp13Us = 0; // 31 10 01 13 — radio prosi NAS o ekran
static uint16_t statDisp13_3B = 0;// 3B 10 01 13 — radio prosi 3B (wewn. CD) o ekran
static uint16_t statPing12Us = 0; // 01 12 do nas — zapytanie o status
static uint16_t statBtn = 0;      // komendy panelu (tad 0x11)
static uint16_t statBreak = 0;    // ile breakow wystawilismy
static uint16_t statText = 0;     // 0x84 — zadanie tekstu (nazwy) do nas
static uint16_t statSeek = 0;     // 0x24/0x25 — FF/REW do nas

// ============================================================
// API
// ============================================================
void begin() {
    lastPingTime = millis();
}

bool isAllocated() {
    return deviceAllocated;
}

// Wewnetrzny pomocnik: kolejkuje gotowa ramke do nadania po grancie 0x13.
static void enqueueFrame(uint8_t priority, const uint8_t* bytes, int len) {
    if (bytes == nullptr || len <= 0) return;
    if (len > Tx::TX_ITEM_MAX_BYTES) len = Tx::TX_ITEM_MAX_BYTES;
    txQueue.enqueue(priority, bytes, (uint8_t)len);
}

// Publiczne API kolejkowania (uzywane przez inne moduly: CD-TEXT, magazynek,
// ikony, ramka czasu). Time-poll PONG (0x01 0x12) NIE przechodzi przez kolejke.
void enqueue(uint8_t priority, const uint8_t* bytes, int len) {
    enqueueFrame(priority, bytes, len);
}

void serviceStats(unsigned long now) {
    if (now - lastStatMs < 2000) return;
    lastStatMs = now;
    // Wypisuj tylko gdy cos sie dzieje (nie zasmiecaj gdy magistrala cicha).
    if (statPoll15 || statDisp13Us || statDisp13_3B || statPing12Us || statBtn || statBreak) {
        Serial.printf("[STAT] poll15=%u disp13(my)=%u disp13(3B)=%u ping12(my)=%u btn=%u break=%u txt=%u seek=%u | alloc=%d state=0x%02X CD%d TR%d %02d:%02d\n",
                      statPoll15, statDisp13Us, statDisp13_3B, statPing12Us, statBtn, statBreak,
                      statText, statSeek,
                      deviceAllocated ? 1 : 0, statusByteFromState(CdChanger::mechState()),
                      CdChanger::disk(), CdChanger::track(),
                      CdChanger::minutes(), CdChanger::seconds());
    }
    statPoll15 = statDisp13Us = statDisp13_3B = statPing12Us = statBtn = statBreak = 0;
    statText = statSeek = 0;
}

// ------------------------------------------------------------
// Pelny reset stanu sesji (wspolny rdzen dla BUS-off).
// ------------------------------------------------------------
void onBusOff() {
    // Zdarzenie Start (cykl zycia) -> {0x30, false} (R4.1, R4.4, R4.5).
    setAddrState(AddressManager::apply(addrState(), AddressManager::Event::Start, 0));
    // Reset markerow preliminary (ale NIE isCdxM670 — to zostaje po wykryciu,
    // zeby przezyc cykl BUS_ON).
    lastPreliminaryTime = 0;
    anyoneIgnoredCount = 0;
    resetLoopCount = 0;
}

bool serviceTimeout(unsigned long now) {
    if (deviceAllocated && (now - lastPingTime > RADIO_TIMEOUT_MS)) {
        // Radio zniklo — wracamy do stanu startowego (R4.1, R4.5).
        setAddrState(AddressManager::apply(addrState(), AddressManager::Event::Start, 0));
        return true;
    }
    return false;
}

void serviceSlaveBreak(bool busPowered) {
    // Radio pollu­je nas o ekran (01 13) tylko ~0.3Hz samo z siebie, a czas ma
    // sie odswiezac ~1Hz. Slave Break to nasza dzwignia: budzi radio, by pobralo
    // ekran. Dwa tryby:
    //   - PUSH (isDisplayDirty): natychmiast po realnej zmianie (utwor/plyta),
    //     odstep BREAK_INTERVAL_MS — szybki feedback klawiszy.
    //   - KEEPALIVE: gdy radio nie pobralo ekranu od >DISPLAY_KEEPALIVE_MS,
    //     budzimy je, odstep DISPLAY_KEEPALIVE_MS => maks ~2Hz (bez sztormu,
    //     ktory przy ~6.7Hz dawal SYSTEM RESET).
    // Break sam sie chroni (porzuca, gdy radio ruszy zegarem). Po wystawieniu
    // kasujemy dirty (push obsluzony).
    if (!busPowered || !deviceAllocated) return;
    // Break dozwolony w stanach "aktywnego ekranu CD": Playing oraz przejsciowych
    // (ladowanie/zmiana plyty, seek). Dzieki temu po nacisnieciu klawisza radio
    // jest budzone NATYCHMIAST (nie czeka ~100ms az wrocimy do Playing) — szybszy
    // feedback. Blokujemy tylko stany, w ktorych nie ma czego pokazywac na ekranie
    // czasu (Init/Idle/Ejecting).
    {
        CdChanger::MechState ms = CdChanger::mechState();
        bool activeForDisplay = (ms == CdChanger::MechState::Playing      ||
                                 ms == CdChanger::MechState::LoadingTrack ||
                                 ms == CdChanger::MechState::ChangedCd    ||
                                 ms == CdChanger::MechState::Changing     ||
                                 ms == CdChanger::MechState::Seeking);
        if (!activeForDisplay) return;
    }

    bool silToBreak = (UnilinkBus::microsSinceLastClock() > BREAK_SILENCE_US);
    if (!silToBreak || UnilinkBus::isTransmitting()) return;

    unsigned long nowMs = millis();
    if (nowMs < suppressBreakUntil) return;

    // Wyzwalacze Slave Break:
    //   - change : realna zmiana ekranu (utwor/plyta) — szybki feedback (PUSH),
    //   - stale  : radio dawno nie pobralo ekranu (keepalive),
    //   - queued : w kolejce TX czeka ramka (status/disc ID/info/nazwa/czas) i
    //              trzeba sprowokowac grant 0x13, by ja wyslac.
    // Kolejka TX traktowana jak PUSH (szybki odstep BREAK_INTERVAL_MS).
    bool change = CdChanger::isDisplayDirty();
    bool stale  = (nowMs - lastDisplayServedMs > DISPLAY_KEEPALIVE_MS);
    bool queued = !txQueue.isEmpty();
    if (!change && !stale && !queued) return;

    // [HIGH-RISK] Stale czasowe. `change` (realna zmiana ekranu: klawisz / nowy
    // utwor / tik sekundy) dostaje SZYBKI odstep ~40ms, by feedback klawiszy byl
    // niemal natychmiastowy. To NIE jest sztorm: `change` (dirty) ustawiany jest
    // przy DYSKRETNYCH zdarzeniach (raz na sekunde dla czasu, raz na nacisniecie)
    // i kasowany po wystawieniu break — wiec realnie ~1 break/s + 1 na klawisz.
    // Sztorm, ktory dawal SYSTEM RESET, bral sie z `queued` trzymanego stale przy
    // zalegajacej kolejce (juz nie wystepuje — czas budujemy swiezo na grant).
    // Naturalny prog to i tak BREAK_SILENCE_US (8ms ciszy) wymagany nizej.
    const unsigned long BREAK_CHANGE_FAST_MS = 40;
    unsigned long minInterval;
    if (change)      minInterval = BREAK_CHANGE_FAST_MS;   // szybki feedback klawiszy/czasu
    else if (queued) minInterval = BREAK_INTERVAL_MS;      // kolejka (CD-TEXT/disc ID)
    else             minInterval = DISPLAY_KEEPALIVE_MS;   // keepalive
    if (nowMs - lastBreakTime < minInterval) return;

    UnilinkBus::issueSlaveBreak();
    lastBreakTime = nowMs;
    statBreak++;
    if (DEBUG_FRAMES) {
        Serial.printf("   BREAK (change=%d stale=%d queued=%d)\n",
                      change ? 1 : 0, stale ? 1 : 0, queued ? 1 : 0);
    }
    CdChanger::clearDisplayDirty();
}

// ------------------------------------------------------------

// ------------------------------------------------------------
// CD-TEXT: zbuduj i ZAKOLEJKUJ jedno pole nazwy (utwor lub plyta).
// ------------------------------------------------------------
// Zadanie radia (`0x84 0xD9` utwor / `0x84 0xDD` plyta) niesie wylacznie NUMER
// POLA w D1 — Kompendium NIE koduje w nim konkretnej plyty/utworu. ZALOZENIE:
// nazwa dotyczy BIEZACEJ selekcji zmieniarki, wiec zrodlo bierzemy z
// CdChanger::disk()/track() (R6.1, R6.2).
//
// Budowa ramki nazwy (long, wariant 8-znakowy, Kompendium §10.2, design §6):
//   {0x70, myAddr, CMD1, c0, P1, c1..c7, 0x00, pole, P2, 0x00}
// czyli 8 znakow pola trafia w CMD2 (c0) + D1..D7 (c1..c7), D8=0x00, D9=pole.
// CMD1 wg CdText::commandForField(pole, isDisc): pola 0-1 -> 0xC9/0xCD,
// pola 2-5 -> 0xD9/0xDD (R6.3, R6.4).
// Struktura ramki:
//   Byte 0-3: RAD TAD CMD1 CMD2
//   Byte 4:   Parity1 = (RAD+TAD+CMD1+CMD2) mod 256
//   Byte 5-13: D1..D9 = dane (znaki + separator + numer pola)
//   Byte 14:  Parity2 = (Parity1 + D1..D9) mod 256
//   Byte 15:  END = 0x00
// [HIGH-RISK] Struktura ramki (dlugosc 16B, parzystosci) jest zgodna z
// Kompendium §10.2. Zmiana struktury ramki (np. zmiana pozycji danych) wymaga
// aktualizacji Parity1/Parity2 wedlug wzorow w UnilinkFrame.h.
//
// Budowa ramki nazwy (long, wariant 8-znakowy, Kompendium §10.2, design §6):
//   {0x70, myAddr, CMD1, c0, P1, c1..c7, 0x00, pole, P2, 0x00}
// czyli 8 znakow pola trafia w CMD2 (c0) + D1..D7 (c1..c7), D8=0x00, D9=pole.
// CMD1 wg CdText::commandForField(pole, isDisc): pola 0-1 -> 0xC9/0xCD,
// pola 2-5 -> 0xD9/0xDD (R6.3, R6.4).
//
// [WARIANT_PROTOKOLU §6 (R6.7, R1.2)] Dostepny jest tez wariant 0xD2 (6 znakow,
// pierwszy znak w CMD2 — CdText::buildFieldD2). Wybor nalezy do kontekstu
// komunikacji; gdy radio nie sygnalizuje trybu CD, domyslnie uzywamy wariantu
// 8-znakowego 0xC9/0xD9 (pragmatyczny default — patrz Kompendium §10.3).
static void enqueueCdTextField(bool isDisc, int field) {
    // R6.5: pole > 5 konczy przesylanie — nic nie kolejkujemy.
    if (field < 0 || field > CdText::MAX_FIELD) return;

    // Zrodlo nazwy: biezaca selekcja zmieniarki (patrz ZALOZENIE wyzej).
    char raw[64];
    if (isDisc) {
        audioGetDiscName(CdChanger::disk(), raw, sizeof(raw));
    } else {
        audioGetTrackName(CdChanger::disk(), CdChanger::track(), raw, sizeof(raw));
    }

    // R6.6: sanityzacja zrodla do drukowalnego ASCII przed podzialem na pola.
    char name[64];
    CdText::sanitizeAscii(raw, name, sizeof(name));

    // R6.5: koniec tekstu (offset pola poza dlugoscia nazwy) konczy przesylanie.
    if (!CdText::fieldExists(name, field, CdText::FIELD8_CHARS)) return;

    // 8 znakow pola: c0 -> CMD2, c1..c7 -> D1..D7 (buildField8 dopelnia spacja).
    uint8_t chars[CdText::FIELD8_CHARS];
    CdText::buildField8(name, field, chars);

    uint8_t cmd1 = CdText::commandForField(field, isDisc);

    // Zloz kompletna ramke long (16B) z parzystosciami (gotowa do nadania).
    uint8_t frame[16];
    frame[0]  = 0x70;                      // RAD = wyswietlacz
    frame[1]  = myAddr;                    // TAD = nasz adres
    frame[2]  = cmd1;                      // CMD1 = 0xC9/0xCD/0xD9/0xDD
    frame[3]  = chars[0];                  // CMD2 = znak 0
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = chars[1];                  // D1 = znak 1
    frame[6]  = chars[2];                  // D2 = znak 2
    frame[7]  = chars[3];                  // D3 = znak 3
    frame[8]  = chars[4];                  // D4 = znak 4
    frame[9]  = chars[5];                  // D5 = znak 5
    frame[10] = chars[6];                  // D6 = znak 6
    frame[11] = chars[7];                  // D7 = znak 7
    frame[12] = 0x00;                      // D8 = 0x00 (separator §10.2)
    frame[13] = (uint8_t)field;            // D9 = numer pola
    frame[14] = UnilinkFrame::parity2(frame[4], &frame[5], 9);  // P2 nad D1..D9
    frame[15] = 0x00;                      // END

    // Odpowiedz kolejkowana (R6.1/R6.2) — wysylana po grancie 0x13.
    enqueue(Tx::PRIO_CD_TEXT, frame, sizeof(frame));
}

// ------------------------------------------------------------
// CD-TEXT wariant CDX-M670: zadanie `0x84 0xD7`, odpowiedz `0xD2` (Kompendium
// §10.3/§10.4). Odtworzone 1:1 z realnego sniffu CDX-M670:
//   zadanie : 31 71 84 D7 FD 70 00 00 01 6E 00   (op2=0xD7, tad=0x71=proc.ekranu)
//   odpowiedz: 70 31 D2 4B BE 61 70 69 74 61 02 00 F1 10 D0 00  ("Kapita")
//             70 31 D2 6E E1 73 6B 69 65 20 74 01 F1 10 23 00  ("nskie…")
// Format ramki 0xD2 (long, 16B): pierwszy znak w CMD2, kolejne 5 w D1..D5
// (6 znakow na ramke), D6=0x00, D7=numer pola, D8=numer plyty F|nr, D9=0x10.
// Radio NIE podaje numeru pola w zadaniu (D-bajty stale) — wysyla te samo
// zadanie wielokrotnie, a ZMIENIARKA STRUMIENIUJE kolejne pola. Trzymamy wiec
// wlasny licznik pola, zerowany przy zmianie plyty/utworu; po wyczerpaniu nazwy
// zawijamy do pola 0 (radio sklada nazwe wg numeru pola w D7).
//
// [WARIANT_PROTOKOLU §10.3] To jest format uzywany przez CDX-M670. Starszy
// handler 0xC9/0xD9 (8 znakow) pozostaje dla radii uzywajacych wariantu §10.2.
static uint8_t s_d2Field      = 0;
static uint8_t s_d2ForDisc    = 0;
static uint8_t s_d2ForTrack   = 0;

static void enqueueCdTextD2Track() {
    uint8_t disc  = CdChanger::disk();
    uint8_t track = CdChanger::track();

    // Reset strumienia przy zmianie selekcji (nowa plyta/utwor => od pola 0).
    if (disc != s_d2ForDisc || track != s_d2ForTrack) {
        s_d2ForDisc  = disc;
        s_d2ForTrack = track;
        s_d2Field    = 0;
    }

    char raw[64];
    audioGetTrackName(disc, track, raw, sizeof(raw));
    char name[64];
    CdText::sanitizeAscii(raw, name, sizeof(name));

    // Po wyczerpaniu nazwy zawijamy do pola 0 (radio i tak sklada wg D7).
    if (!CdText::fieldExists(name, s_d2Field, CdText::FIELDD2_CHARS)) {
        s_d2Field = 0;
        if (!CdText::fieldExists(name, 0, CdText::FIELDD2_CHARS)) return;  // pusta nazwa
    }

    uint8_t chars[CdText::FIELDD2_CHARS];   // 6 znakow (pierwszy -> CMD2)
    CdText::buildFieldD2(name, s_d2Field, chars);

    uint8_t frame[16];
    frame[0]  = 0x70;                          // RAD = wyswietlacz
    frame[1]  = myAddr;                         // TAD = nasz adres
    frame[2]  = 0xD2;                           // CMD1 = 0xD2 (CD-TEXT tryb CD)
    frame[3]  = chars[0];                       // CMD2 = znak 0
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = chars[1];                       // D1 = znak 1
    frame[6]  = chars[2];                       // D2 = znak 2
    frame[7]  = chars[3];                       // D3 = znak 3
    frame[8]  = chars[4];                       // D4 = znak 4
    frame[9]  = chars[5];                       // D5 = znak 5
    frame[10] = 0x00;                           // D6 (rezerwa)
    frame[11] = s_d2Field;                      // D7 = numer pola (sklejanie w radiu)
    frame[12] = UnilinkFrame::encodeDiscNibble(disc);  // D8 = plyta F|nr
    frame[13] = 0x10;                           // D9 (stale w sniffie CDX-M670)
    frame[14] = UnilinkFrame::parity2(frame[4], &frame[5], 9);
    frame[15] = 0x00;                           // END

    enqueue(Tx::PRIO_CD_TEXT, frame, sizeof(frame));
    s_d2Field++;   // nastepne zadanie -> kolejne pole (strumien)
}

// ------------------------------------------------------------
// MAGAZYNEK: ADRESOWANIE ODPOWIEDZI (RAD)
// ------------------------------------------------------------
// Ramki magazynka (0x95 mapa / 0x97 info) i disc ID (0xD5) adresujemy RAD=0x70
// (wyswietlacz) — SPÓJNIE z pozostalymi ramkami kolejkowanymi w TxQueue: status
// odtwarzania 0xC0 (sendDisplayStatus) i nazwy CD-TEXT (enqueueCdTextField)
// rowniez uzywaja RAD=0x70. Wszystkie te odpowiedzi schodza z kolejki po tym
// samym grancie request-poll `0x01 0x13` (kierowanym do nas: rad=myAddr,
// tad=0x10), wiec adresujemy je jednolicie jak inne ramki ekranowe. TAD=myAddr
// (nasz przydzielony adres) we wszystkich odpowiedziach. (design.md §7,
// Kompendium §8.2 — disc/magazyn info odsylane do warstwy ekranu radia.)
static constexpr uint8_t MAGAZINE_RAD = 0x70;  // RAD = wyswietlacz (jak 0xC0 / CD-TEXT)

// ------------------------------------------------------------
// Magazynek: zbuduj i ZAKOLEJKUJ ramke middle (0x95 mapa / 0x97 info plyty).
// data4 = 4 bajty danych (D1..D4). CMD1 0x80..0xBF => ramka middle (11B).
// ------------------------------------------------------------
static void enqueueMagazineMiddle(uint8_t cmd1, const uint8_t* data4) {
    uint8_t frame[11];
    frame[0]  = MAGAZINE_RAD;          // RAD = wyswietlacz
    frame[1]  = myAddr;                // TAD = nasz adres
    frame[2]  = cmd1;                  // CMD1 = 0x95 / 0x97 (middle)
    frame[3]  = 0x00;                  // CMD2
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = data4[0];              // D1
    frame[6]  = data4[1];              // D2
    frame[7]  = data4[2];              // D3
    frame[8]  = data4[3];              // D4
    frame[9]  = UnilinkFrame::parity2(frame[4], &frame[5], 4);  // P2 nad D1..D4
    frame[10] = 0x00;                  // END
    enqueue(Tx::PRIO_MAGAZINE, frame, sizeof(frame));
}

// ------------------------------------------------------------
// Magazynek: zbuduj i ZAKOLEJKUJ ramke disc ID 0xD5 (long, 9 bajtow danych).
// ------------------------------------------------------------
// Wywolywane przy skanie magazynka / zmianie plyty (R7.3). Disc ID jest staly
// dla danej plyty miedzy zadaniami (R7.5) — Magazine::buildDiscId cache'uje go.
// CMD1 >= 0xC0 => ramka long (16B). Publiczne API, aby kod wykrywajacy zmiane
// plyty (inne zadania w CdChanger / wybor 0xB0) mogl je wywolac.
//
// [WIRING] Wpiecie w detekcje zmiany plyty zyje w module CdChanger (nextDisc/
// prevDisc/0xB0), ktory nalezy do innych zadan — tutaj wystawiamy gotowy helper
// (deklaracja w UnilinkProtocol.h), by te zadania mogly go wywolac bez
// duplikowania logiki budowania ramki.
void enqueueDiscId(uint8_t disc) {
    uint8_t d[Magazine::DISC_ID_DATA_LEN];
    Magazine::buildDiscId(disc, d);

    uint8_t frame[16];
    frame[0]  = MAGAZINE_RAD;          // RAD = wyswietlacz
    frame[1]  = myAddr;                // TAD = nasz adres
    frame[2]  = 0xD5;                  // CMD1 = 0xD5 (long, disc ID)
    frame[3]  = 0x00;                  // CMD2
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    for (int i = 0; i < Magazine::DISC_ID_DATA_LEN; ++i) {
        frame[5 + i] = d[i];           // D1..D9 = ID plyty + F|nr
    }
    frame[14] = UnilinkFrame::parity2(frame[4], &frame[5], Magazine::DISC_ID_DATA_LEN);  // P2 nad D1..D9
    frame[15] = 0x00;                  // END
    enqueue(Tx::PRIO_DISC_ID, frame, sizeof(frame));
}

// ------------------------------------------------------------
// IKONY TRYBOW: zbuduj i ZAKOLEJKUJ ramke 0x94 (Repeat/Shuffle/Intro, R8.4).
// ------------------------------------------------------------
// Po KAZDEJ zmianie trybu (0x34/0x35/0x36) odzwierciedlamy biezacy stan trybow
// w ramce ikon `0x94`. CMD1 0x94 lezy w 0x80..0xBF => ramka middle (11B,
// 4 bajty danych D1..D4). Kodowanie stanu <-> bajty danych zyje w czystym module
// UnilinkFrame (encodeIconData/decodeIconData) — round-trippable i testowalne na
// hoscie (Property 15 / zadanie 11.5). CdChanger pozostaje wolny od wiedzy o
// ramkach UniLink; budowa ramki nalezy do tej warstwy.
//
// [PRIORYTET] design.md nie wymienia osobnego priorytetu dla ikon. Ikony sa
// informacja statusowa zblizona charakterem do info magazynka (oba to ramki
// middle 0x9X z biezacym stanem urzadzenia), wiec uzywamy Tx::PRIO_MAGAZINE.
// Adresowanie jak inne ramki ekranowe: RAD=0x70 (wyswietlacz), TAD=myAddr.
// Zawiera kodowanie stanu trybow w D1 (shuffle bit0, intro bit1, repeat w bity 4-5).
void enqueueModeIconsHelper(uint8_t repeatMode, bool shuffle, bool intro) {
    uint8_t data[UnilinkFrame::ICON_DATA_LEN];
    UnilinkFrame::encodeIconData(repeatMode, shuffle, intro, data);

    uint8_t frame[11];
    frame[0]  = MAGAZINE_RAD;          // RAD = wyswietlacz (jak 0xC0 / 0x95 / CD-TEXT)
    frame[1]  = myAddr;                // TAD = nasz adres
    frame[2]  = 0x94;                  // CMD1 = 0x94 (middle, ikony trybow)
    frame[3]  = 0x00;                  // CMD2
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = data[0];               // D1 = bity Repeat/Shuffle/Intro
    frame[6]  = data[1];               // D2 = rezerwa
    frame[7]  = data[2];               // D3 = rezerwa
    frame[8]  = data[3];               // D4 = rezerwa
    frame[9]  = UnilinkFrame::parity2(frame[4], &frame[5], UnilinkFrame::ICON_DATA_LEN);  // P2 nad D1..D4
    frame[10] = 0x00;                  // END
    enqueue(Tx::PRIO_MAGAZINE, frame, sizeof(frame));
}

// Wewnetrzna aliasowa do enqueueModeIconsHelper dla wygody w handlePacket
// (zadanie 8.6 - wywolywane przy komendach 0x34/0x35/0x36).
static inline void enqueueModeIcons() {
    enqueueModeIconsHelper((uint8_t)CdChanger::repeatMode(),
                           CdChanger::shuffle(),
                           CdChanger::intro());
}

// Deklaracja wyprzedzajaca — definicja nizej (sekcja ramki 0xC0). Uzywana przez
// servicePositionFrame1Hz (odswiezanie 1 Hz) oraz enqueueFullStatusFrame.
static void enqueueStatusC0(uint8_t prio);
// Buduje i NADAJE natychmiast swieza ramke 0xC0 (uzywane na grant 0x13 gdy
// kolejka pusta). Definicja nizej (sekcja ramki 0xC0).
static void sendFreshStatusC0();
// Wybiera i nadaje swiezy ekran na grant 0x13 (0x90 tik w Playing / 0xC0 przy
// zmianie plyty-utworu). Definicja nizej.
static void sendFreshDisplay();

// ============================================================
// POMOCNIKI DO RAMKI POZYCJI 0x90 (Wymaganie 11 - Zadanie 14.1)
// ============================================================
// Rama 0x90 wysyłana co sekundę w stanie Playing. Dwa warianty:
//   - CMD2=0x30 (lekki tik): D1=F|nr, D3=sekundy (jak CDX-805 sniff)
//   - CMD2=0x50 (pełny): D1=utwór, D2=minuty, D3=sekundy, D4=F|nr
// Adresowanie: RAD=0x70 (wyswietlacz), TAD=myAddr, priorytet Tx::PRIO_TIME (4).
// Wstrzymuje inkrementację w non-playing states (Seeking/ChangedCd/LoadingTrack).
//
// Struktura ramki 0x90 (middle, 11B):
//   Byte 0-3: RAD TAD CMD1 CMD2
//   Byte 4:   Parity1 = (RAD+TAD+CMD1+CMD2) mod 256
//   Byte 5-8: D1..D4 = dane (varia wg CMD2)
//   Byte 9:   Parity2 = (Parity1 + D1..D4) mod 256
//   Byte 10:  END = 0x00
// [HIGH-RISK] Struktura ramki 0x90 (długość 11B, parzystości, D1..D4) jest
// zgodna z Kompendium §11.2/§11.3. Zmiana struktury ramki (np. zmiana pozycji
// danych) wymaga aktualizacji Parity1/Parity2 wedlug wzorow w UnilinkFrame.h.

// Zbuduj i zakolejkuj ramkę 0x90 z wariantem CMD2=0x30 (lekki tik).
// D1=F|nr płyty, D2=0xF0 (marker), D3=sekundy, D4=bajt stanu ( jak w CDX-805 sniff).
// Używana przy każdej sekundzie w stanie Playing.
void enqueuePositionFrameLightTick() {
    uint8_t trk = CdChanger::track();
    uint8_t min = CdChanger::minutes();
    uint8_t sec = CdChanger::seconds();
    uint8_t dsk = CdChanger::disk();

    // Sekundy bez F-paddingu (00-59)
    uint8_t secBCD   = ((sec / 10) << 4) | (sec % 10);
    // Numer płyty w D1 jako F|nr
    uint8_t discByte = UnilinkFrame::encodeDiscNibble(dsk);
    // bajt stanu: 0x80 (marker) + 6 najmłodszych bitów to statusCdChanger
    // (0x80 + 0x00=Playing, 0x00=seeking itp. - jak w sniffie)
    uint8_t statusByte = (CdChanger::mechState() == CdChanger::MechState::Playing) ? 0x80 : 0x00;

    // Ramka middle (11B): RAD TAD CMD1 CMD2 P1 D1 D2 D3 D4 P2 0
    // CMD2=0x30: D1=F|nr, D2=0xF0 (marker), D3=seconds, D4=status byte
    uint8_t frame[11];
    frame[0]  = 0x70;                      // RAD = wyswietlacz
    frame[1]  = myAddr;                    // TAD = nasz adres
    frame[2]  = 0x90;                      // CMD1 = 0x90 (middle, pozycja)
    frame[3]  = 0x30;                      // CMD2 = 0x30 (lekki tik)
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = discByte;                  // D1 = F|nr (numer płyty)
    frame[6]  = 0xF0;                      // D2 = 0xF0 (marker)
    frame[7]  = secBCD;                    // D3 = seconds BCD
    frame[8]  = statusByte;                // D4 = status byte
    frame[9]  = UnilinkFrame::parity2(frame[4], &frame[5], 4);  // P2 nad D1..D4
    frame[10] = 0x00;                      // END

    enqueue(Tx::PRIO_TIME, frame, sizeof(frame));
}

// Zbuduj i zakolejkuj ramkę 0x90 z wariantem CMD2=0x50 (pełny).
// Utwór/minuty/sekundy/płyta w D1-D4.
// Używana przy zmianie stanu/dysku/utworu dla natychmiastowej aktualizacji.
void enqueuePositionFrameFull() {
    uint8_t trk = CdChanger::track();
    uint8_t min = CdChanger::minutes();
    uint8_t sec = CdChanger::seconds();
    uint8_t dsk = CdChanger::disk();

    // Klasyczne BCD kodowanie
    uint8_t trackBCD = ((trk / 10) << 4) | (trk % 10);
    uint8_t minBCD   = ((min / 10) << 4) | (min % 10);
    uint8_t secBCD   = ((sec / 10) << 4) | (sec % 10);
    // Numer płyty jako F|nr (high nibble F, low nibble disc nr)
    uint8_t discByte = UnilinkFrame::encodeDiscNibble(dsk);

    // Ramka middle (11B)
    uint8_t frame[11];
    frame[0]  = 0x70;                      // RAD = wyswietlacz
    frame[1]  = myAddr;                    // TAD = nasz adres
    frame[2]  = 0x90;                      // CMD1 = 0x90 (middle, pozycja)
    frame[3]  = 0x50;                      // CMD2 = 0x50 (pełny format)
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = trackBCD;                  // D1 = track BCD
    frame[6]  = minBCD;                    // D2 = minutes BCD
    frame[7]  = secBCD;                    // D3 = seconds BCD
    frame[8]  = discByte;                  // D4 = disc (F|nr w starszym nibblu)
    frame[9]  = UnilinkFrame::parity2(frame[4], &frame[5], 4);  // P2 nad D1..D4
    frame[10] = 0x00;                      // END

    enqueue(Tx::PRIO_TIME, frame, sizeof(frame));
}

// Harmonogram 1Hz — ZACHOWANY dla zgodnosci API, ale CELOWO NIE pre-kolejkuje
// juz ramek czasu. Ramka 0xC0 jest budowana SWIEZO na grant 0x13 (patrz sekcja 5
// handlePacket), wiec radio zawsze dostaje aktualny czas/utwor/plyte. Wczesniejsze
// pre-kolejkowanie co sekunde powodowalo:
//   - zaleganie nieaktualnych ramek przy wolniejszym odpytywaniu (miganie plyty
//     CD01->CD02->CD01, opoznione reakcje),
//   - utrzymywanie txQueue niepustej => Slave Break wyzwalany flaga `queued`
//     nawet co BREAK_INTERVAL_MS (150ms) => sztorm break => SYSTEM RESET radia.
// Plynny licznik utrzymuje CdChanger::update() (z playBaseMs); radio dolicza
// sekundy lokalnie miedzy odpytaniami (UNILINK_PROTOKOL.md §7).
void servicePositionFrame1Hz(unsigned long now) {
    (void)now;
    // celowo puste — patrz komentarz wyzej
}

// ============================================================
// Glowny dyspozytor ramek
// ============================================================
void handlePacket(const uint8_t* buf, int len) {
    if (len < 6) return;

    // Czarna skrzynka: rejestrujemy KAZDA ramke (tez przeklamana) — to ona
    // bywa dowodem kolizji prowadzacej do SYSTEM RESET.
    Diagnostics::recordFrame("RX", buf, len);

    // ===== Walidacja sum kontrolnych calej ramki (Kompendium §4, Wymaganie 2) =====
    // Pojedyncze sprawdzenie buf[4] (Parity1) zastapione pelna walidacja:
    // dlugosc wg CMD1, Parity1, Parity2 (middle/long) oraz bajt koncowy.
    // Ramka niezgodna jest ODRZUCANA i rejestrowana jako zdarzenie "DROP" —
    // NIE zmienia stanu sesji ani zmieniarki (R2.1–R2.5).
    if (UnilinkFrame::validate(buf, len) != UnilinkFrame::ValidateResult::Ok) {
        Diagnostics::recordNote("DROP");
        return;
    }

    lastPingTime = millis();

    uint8_t rad = buf[0];
    uint8_t tad = buf[1];
    uint8_t op1 = buf[2];
    uint8_t op2 = buf[3];

    // ===== Diagnostyka: zliczanie kluczowych ramek =====
    if (rad == ADDR_BROADCAST && op1 == 0x01 && op2 == 0x15) statPoll15++;
    else if (rad == myAddr && op1 == 0x01 && op2 == 0x13)     statDisp13Us++;
    else if (rad == 0x3B && op1 == 0x01 && op2 == 0x13)       statDisp13_3B++;
    if (rad == myAddr && op1 == 0x01 && op2 == 0x12)          statPing12Us++;
    if (rad == myAddr && tad == 0x11)                         statBtn++;
    if (rad == myAddr && op1 == 0x84)                         statText++;
    if (rad == myAddr && (op1 == 0x24 || op1 == 0x25))        statSeek++;

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
    // Device info 0x8C wysylamy TYLKO gdy nie mamy jeszcze ID (R4.2). Inaczej
    // radio przydzieliloby nam drugi/trzeci adres, biorac nas za nowe urzadzenie.
    if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x02) {
        if (AddressManager::shouldSendDeviceInfo(addrState(), AddressManager::Event::Anyone)) {
            // [WARIANT_PROTOKOLU / DEVIATION §6 (R12)] Kwirk CDX-M670: ignoruj
            // ANYONE? w oknie preliminary — to faza dla wewnetrznych urzadzen
            // radia, nie dla nas. Wariant discovery, NIE sprzeczny z adopcja 0x3X.
            if (isCdxM670 && lastPreliminaryTime != 0 &&
                (millis() - lastPreliminaryTime) < PRELIMINARY_WINDOW_MS) {
                anyoneIgnoredCount++;
                Serial.printf(">> [CDX-M670] Ignoruje ANYONE? w oknie preliminary (#%d, %lums po DB)\n",
                              anyoneIgnoredCount, millis() - lastPreliminaryTime);
                return;
            }

            // DEVICE INFO 0x8C — atrybuty zgodne z prawdziwa zmieniarka (CDX-M670
            // sniff). TAD = 0x30 = myAddr w stanie nieprzydzielonym (R4.2):
            //   10 30 8C D0 | 9C | 05 A8 1F A3 | 0B 00
            const uint8_t attr[] = {0x10, myAddr, 0x8C, 0xD0, 0x9C, 0x05, 0xA8, 0x1F, 0xA3, 0x0B, 0x00};
            UnilinkBus::sendRaw(attr, sizeof(attr));
            Serial.println(">> Odpowiadam na ANYONE? device info 0x8C");
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
            CdChanger::resetToInit();
            CdChanger::clearDisplayDirty();
        } else {
            Serial.printf(">> Radio system reset (#%d) (juz bylem nieprzydzielony)\n", resetLoopCount);
        }
        // Bus reset -> {0x30, false} (R4.4). Nastepny przydzial moze byc inny.
        setAddrState(AddressManager::apply(addrState(), AddressManager::Event::BusReset, rad));
        lastPreliminaryTime = 0;
        anyoneIgnoredCount = 0;
    }

    // ===== 2. ADDRESS APPOINT (3X 10 02 XX) =====
    // Radio przydziela adres z grupy CD. Adoptujemy DOWOLNE ID z grupy
    // (RAD & 0xF0)==0x30, czyli 0x30..0x3F — nie tylko 0x31..0x3A (R4.3,
    // Kompendium §6.2). op2 i sam adres roznia sie miedzy radiami.
    else if (tad == ADDR_MASTER && op1 == 0x02 && AddressManager::isCdGroup(rad)) {
        setAddrState(AddressManager::apply(addrState(), AddressManager::Event::Appoint, rad));
        CdChanger::resetToInit();
        // POTWIERDZENIE device info 0x8C, TAD = myAddr (R4.3). Atrybuty zgodne z
        // prawdziwa zmieniarka (sniff, adres 0x31):
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
        CdChanger::MechState ms = CdChanger::mechState();
        if (ms == CdChanger::MechState::Playing ||
            ms == CdChanger::MechState::Seeking) {
            displayType = 0x04;  // Playing/FF-REW — pelny ekran z track/czas
        } else if (ms == CdChanger::MechState::Changing ||
                   ms == CdChanger::MechState::LoadingTrack ||
                   ms == CdChanger::MechState::ChangedCd) {
            displayType = 0x05;  // zmiana/ladowanie plyty — ekran przejsciowy
        } else {
            displayType = 0x01;  // Init/Idle/Ejecting — ekran startowy
        }
        UnilinkBus::sendMedium(0x10, 0x18, 0x82, displayType, 0x00, 0x00, 0x00, 0x00);
        if (DEBUG_VERBOSE) {
            Serial.printf(">> Odpowiadam na 01 15 (Slave Poll): chce ekran typ 0x%02X (status=0x%02X)\n",
                          displayType, statusByteFromState(CdChanger::mechState()));
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
        // Odpowiedz: <pytajacy> <addr> 00 <statusByte> <parity> 00
        // [DEVIATION §7.1] Init->0x80 (Idle) zamiast starych 0xC0 (Ejecting).
        // Mapowanie zyje w UnilinkFrame::statusByte, tu wykorzystujemy helper.
        UnilinkBus::sendShort(tad, myAddr, 0x00, statusByteFromState(CdChanger::mechState()));
        if (DEBUG_VERBOSE) {
            Serial.printf(">> PING odpowiedz (do 0x%02X): status=0x%02X\n", tad, statusByteFromState(CdChanger::mechState()));
        }
    }

    // ===== 5. UPDATE DISPLAY (3X 10 01 13) =====
    // TYLKO gdy tad == 0x10 (pytanie do nas). NIE odpowiadamy na 31 14 01 13 —
    // to pytanie do display processora (0x14), nie do nas.
    // Grant request-poll: zdejmujemy DOKLADNIE JEDNA ramke z kolejki TX i ja
    // nadajemy (Kompendium §7.2 / §12.4). Po zakonczeniu zadania 14.2 (ramka 0xC0)
    // kolejka jest zawsze pelna (ramki 0x90 i 0xC0 saenqueueowane co sekunde / 30s),
    // wiec sendDisplayStatus() nie jest juz wywolywane.
    else if (rad == myAddr && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x13) {
        lastDisplayServedMs = millis();  // radio wlasnie pobralo nasz ekran
        CdChanger::clearDisplayDirty();
        Tx::TxItem item;
        if (txQueue.dequeue(item)) {
            // Ramka w kolejce jest juz gotowa (z parzystosciami) — nadajemy
            // surowo, niezaleznie od jej dlugosci (short/medium/long).
            UnilinkBus::sendRaw(item.bytes, item.len);
        } else {
            // Kolejka pusta: zbuduj i NADAJ NATYCHMIAST swiezy ekran. W ustalonym
            // Playing to lekki tik 0x90 (radio interpoluje sekundy -> gladki czas);
            // przy zmianie plyty/utworu lub poza Playing -> pelny 0xC0 (numer plyty).
            sendFreshDisplay();
        }
    }

    // ===== 6. WAKE UP / PLAY (3X 10 20 00) =====
    else if (rad == myAddr && tad == ADDR_MASTER && op1 == 0x20 && op2 == 0x00) {
        CdChanger::handlePlayCommand();
    }

    // ===== 6a. FAST FORWARD / REVERSE (3X .. 24/25) =====
    // Opkody wg implementacji referencyjnej (Michael Wolf): 0x24=FF, 0x25=REW.
    // Akceptujemy dowolny TAD (panel 0x11 lub master 0x10).
    else if (rad == myAddr && op1 == 0x24) {
        Serial.printf("[SEEKDBG] t=%lu FF  tad=0x%02X op2=0x%02X len=%d\n",
                      millis(), tad, op2, len);
        CdChanger::seek(+SEEK_STEP_SEC);
    }
    else if (rad == myAddr && op1 == 0x25) {
        Serial.printf("[SEEKDBG] t=%lu REW tad=0x%02X op2=0x%02X len=%d\n",
                      millis(), tad, op2, len);
        CdChanger::seek(-SEEK_STEP_SEC);
    }

    // ===== 6a-alt. STOP SCAN (18 10 08 00) — broadcast Key Off =====
    // Model podstawowy zgodny z Kompendium §9: broadcast 0x08 0x00 (RAD=0x18)
    // konczy przewijanie, zwraca do Playing (0x00) i enqueue nowej pozycji.
    // Fallback: skanowanie zatrzaskowe (seekScanDir/serviceSeekRepeat) pozostaje
    // jako [DEVIATION §9] — gdy radio nie wysyla 0x08. Fallback wykorzystuje
    // SEEK_REPEAT_MS (400ms) i SEEK_SCAN_MAX_MS (30000ms) z Config.h.
    // [HIGH-RISK] SEEK_REPEAT_MS i SEEK_SCAN_MAX_MS: zmiana tych stalych
    // wpływa na zachowanie FF/REW. Aby przywrocic wczesniejsze strojenie:
    // zwieksz SEEK_REPEAT_MS dla wolniejszego skoku, zmnisz SEEK_SCAN_MAX_MS
    // dla szybszego zatrzymania skanu.
    else if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x08 && op2 == 0x00) {
        // Zatrzymaj skanowanie zatrzaskowe (jezeli aktywne). To ustawia dirty,
        // wiec po powrocie do Playing radio dostanie swiezy 0xC0 (budowany na grant).
        // NIE kolejkujemy juz 0xC0 tutaj: radio CDX-M670 wysyla broadcast 0x08
        // BARDZO czesto (nie tylko po seeku), a kazde kolejkowanie robilo kolejke
        // niepusta -> wyzwalacz `queued` w serviceSlaveBreak podbijal liczbe Slave
        // Break do ~6 Hz (ryzyko SYSTEM RESET). Swiezy status i tak idzie na grant.
        CdChanger::stopSeekScan();
        if (DEBUG_VERBOSE) {
            Serial.println(">> 0x08 0x00 (key off / scan stop)");
        }
    }

    // ===== 6b. TRYBY REPEAT / SHUFFLE / INTRO (3X .. 34/35/36) — Wymaganie 8 =====
    // Komendy przelaczania trybow. Jak FF/REW (0x24/0x25) akceptujemy dowolny TAD
    // (panel 0x11 lub master 0x10) — liczy sie, ze ramka jest adresowana do nas.
    //   0x34 = cykl Repeat Off->One->All->Off (R8.1)
    //   0x35 = toggle Shuffle (R8.2)
    //   0x36 = toggle Intro   (R8.3)
    // Po KAZDEJ zmianie trybu kolejkujemy ramke ikon 0x94 odzwierciedlajaca
    // biezacy stan (R8.4). Budowa ramki nalezy do tej warstwy (enqueueModeIcons),
    // CdChanger pozostaje wolny od wiedzy o ramkach UniLink.
    else if (rad == myAddr && op1 == 0x34) {
        CdChanger::toggleRepeat();
        enqueueModeIcons();
    }
    else if (rad == myAddr && op1 == 0x35) {
        CdChanger::toggleShuffle();
        enqueueModeIcons();
    }
    else if (rad == myAddr && op1 == 0x36) {
        CdChanger::toggleIntro();
        enqueueModeIcons();
    }

    // ===== 7. Komendy nawigacji utwor/plyta (next/prev track / next/prev CD) =====
    // Akceptujemy DOWOLNY TAD (panel 0x11 lub master 0x10) i dowolny op2 — tak jak
    // FF/REW (0x24/0x25). CDX-M670 wysyla 26 10 / 27 10 / 28 00 / 29 00, ale inne
    // radia (lub inne tryby) potrafia uzyc innego TAD/op2. Wczesniejszy warunek
    // `tad==0x11 && op2==0x10` gubil wtedy Track+/- (objaw: "track+ nie dziala").
    else if (rad == myAddr && op1 == 0x26) {
        CdChanger::nextTrack();
    }
    else if (rad == myAddr && op1 == 0x27) {
        CdChanger::prevTrack();
    }
    else if (rad == myAddr && op1 == 0x28) {
        CdChanger::nextDisc();
        enqueueDiscId(CdChanger::disk());   // R7.3: disc ID przy zmianie plyty
    }
    else if (rad == myAddr && op1 == 0x29) {
        CdChanger::prevDisc();
        enqueueDiscId(CdChanger::disk());   // R7.3: disc ID przy zmianie plyty
    }

    // ===== 8. Zadanie CD-TEXT — nazwa utworu (3X .. 84 D9) =====
    // Numer pola w D1 (buf[5]). Odpowiedz (0xC9 pola 0-1 / 0xD9 pola 2-5)
    // kolejkowana w TxQueue i wysylana po grancie 0x13 (R6.1, R6.3, R6.4).
    // Pole > 5 lub koniec tekstu konczy przesylanie (R6.5).
    else if (rad == myAddr && op1 == 0x84 && op2 == 0xD9) {
        enqueueCdTextField(/*isDisc=*/false, buf[5]);
    }

    // ===== 8b. Zadanie CD-TEXT — wariant CDX-M670 (3X .. 84 D7) =====
    // Realny CDX-M670 prosi o nazwe utworu komenda op2=0xD7 (sniff), a NIE 0xD9,
    // i oczekuje odpowiedzi w formacie 0xD2 (6 znakow, 1. znak w CMD2). Radio nie
    // podaje numeru pola — strumieniujemy kolejne pola sami (enqueueCdTextD2Track).
    // [WARIANT_PROTOKOLU §10.3] Wczesniej obslugiwalismy tylko 0xD9/0xDD, przez co
    // CDX-M670 nie dostawal odpowiedzi i CD-TEXT sie nie pokazywal.
    else if (rad == myAddr && op1 == 0x84 && op2 == 0xD7) {
        enqueueCdTextD2Track();
    }

    // ===== 9. Zadanie CD-TEXT — nazwa plyty (3X .. 84 DD) =====
    // Jak wyzej, ale nazwa plyty: 0xCD (pola 0-1) / 0xDD (pola 2-5) (R6.2,
    // R6.3, R6.4, R6.5).
    else if (rad == myAddr && op1 == 0x84 && op2 == 0xDD) {
        enqueueCdTextField(/*isDisc=*/true, buf[5]);
    }

    // ===== 10. Zadanie mapy magazynka (3X .. 84 95) =====
    // Radio prosi o mape obecnosci plyt. Odsylamy ramke 0x95 (middle), w ktorej
    // 16-bitowa mapa Magazine::presenceMap() trafia do D1 (mlodszy bajt) i D2
    // (starszy bajt); D3/D4 = 0. Bit i = plyta (i+1) obecna (R7.1). Odpowiedz
    // kolejkowana (Tx::PRIO_MAGAZINE), nadawana po grancie 0x13.
    else if (rad == myAddr && op1 == 0x84 && op2 == 0x95) {
        uint16_t pm = Magazine::presenceMap();
        uint8_t data[4] = {
            (uint8_t)(pm & 0xFF),          // D1 = mlodszy bajt mapy (plyty 1..8)
            (uint8_t)((pm >> 8) & 0xFF),   // D2 = starszy bajt mapy (plyty 9..16)
            0x00,                          // D3
            0x00                           // D4
        };
        enqueueMagazineMiddle(0x95, data);
    }

    // ===== 10b. Bezposredni wybor płyty (0xB0) — Wymaganie 9 =====
    // Format: RAD TAD 0xB0 disc track (short frame, 6B). CMD2=disc, D1=track.
    // Weryfikacja zakresu przez Magazine/AudioPlayer. Przy poprawnym zakresie
    // ustawia biezaca płyte/utwór, wejdz w SEEK i zakolejkuj zaktualizowany status.
    // Poza zakresem zignoruj bez zmiany stanu.
    else if (rad == myAddr && op1 == 0xB0) {
        uint8_t disc = op2;  // CMD2
        uint8_t track  = buf[4];  // D1

        // Walidacja płyty: 1..10, obecna w magazynku
        bool discValid = (disc >= 1 && disc <= 10) && ((Magazine::presenceMap() >> (disc - 1)) & 1);
        // Walidacja utworu: 1..MAX_TRACKS, w granicach dostępnych na danej plycie
        uint8_t maxTrack = audioGetTrackCount(disc);
        if (maxTrack == 0) maxTrack = MAX_TRACKS;  // fallback bez nosnika
        bool trackValid = (track >= 1 && track <= maxTrack);

        if (discValid && trackValid) {
            // Zmiana plyty/utworu — wejdz w SEEK i zakolejkuj status
            CdChanger::selectDiscTrack(disc, track);
            enqueueDiscId(disc);
            Serial.printf(">> 0xB0: CD%d TR%d (zakres OK)\n", disc, track);
        } else {
            Serial.printf(">> 0xB0: CD%d TR%d (poza zakresem, ignoruje)\n", disc, track);
        }
    }

    // ===== 11. Zadanie info plyty (3X .. 84 97) =====
    // Radio prosi o liczbe utworow + czas calkowity plyty. Numer plyty moze byc
    // w D1 (buf[5]); gdy go brak/0 — uzywamy biezacej plyty (R7.2). Odsylamy
    // ramke 0x97 (middle) z Magazine::buildDiscInfo (D1=utwory, D2=min BCD,
    // D3=sek BCD, D4=F|nr). Odpowiedz kolejkowana (Tx::PRIO_MAGAZINE).
    else if (rad == myAddr && op1 == 0x84 && op2 == 0x97) {
        uint8_t disc = CdChanger::disk();          // domyslnie biezaca plyta
        if (len >= 6 && buf[5] != 0x00) {
            uint8_t d1 = buf[5];
            // D1 moze byc zakodowane jako F|nr (0xF1..) albo surowy numer.
            uint8_t cand = ((d1 & 0xF0) == 0xF0)
                               ? UnilinkFrame::discNibbleToNumber(d1)
                               : d1;
            if (cand >= 1) disc = cand;
        }
        uint8_t data[Magazine::DISC_INFO_DATA_LEN];
        Magazine::buildDiscInfo(disc, data);
        enqueueMagazineMiddle(0x97, data);
    }
}

// ============================================================
// POMOCNIKI DO RAMKI PEŁNEGO STATUSU 0xC0 (Wymaganie 11.4 - Zadanie 14.2)
// ============================================================
// Ramka 0xC0 (long, 16B) z kompletnym stanem odtwarzania: liczba utworów,
// minuty, sekundy, numer płyty. Wysyłana okresowo jako uzupełnienie ramki
// pozycji 0x90 (light tick) wg Kompendium §11.5.
//
// Struktura ramki 0xC0 (long word, Kompendium §3.1):
//   {RAD, TAD, CMD1, CMD2, Parity1, D1, D2, D3, D4, D5, D6, D7, D8, D9, Parity2, 0}
// Gdzie:
//   RAD = 0x70 (display)
//   TAD = myAddr (nasz przydzielony adres)
//   CMD1 = 0xC0 (full status / play status)
//   CMD2 = 0x00
//   D1-D5 = 0x00 (rezerwa)
//   D6 (D2_2) = liczba utworów (0..99)
//   D7 (D2_3) = minuty (BCD, 00..59)
//   D8 (D2_4) = sekundy (BCD, 00..59)
//   D9 (D2_5) = numer płyty (F|nr encoding, np. 0xF1 dla CD1)
//   Parity1/Parity2 = sumy kontrolne

// Zbuduj i zakolejkuj ramkę 0xC0 (long, status odtwarzania) wg UDOKUMENTOWANEGO,
// DZIALAJACEGO formatu z UNILINK_PROTOKOL.md §6 (potwierdzonego sniffem CDX-M670,
// "ten dziala — pokazuje track i czas"):
//
//   70 <addr> C0 00 | P1 | 00 00 00 00 30 <TRK> <MIN> <SEK> <DISC> | P2 | 00
//
// Kodowanie pol:
//   D5 (byte9)  = 0x30  — MARKER strony "ekran odtwarzania" (czas/utwor).
//                 BEZ tego markera radio interpretuje ramke jako LOAD/info plyty
//                 (a NIE jako czas) — patrz ostrzezenie w §6. To byl glowny powod,
//                 dla ktorego "wyswietlacz wariowal / znikala plyta".
//   D6 (byte10) = TRK   — numer utworu, BCD z F-paddingiem (F1..F9 dla 1-9, 10..99)
//   D7 (byte11) = MIN   — minuty, BCD z F-paddingiem (F0 dla 0, F1..F9, 10..99)
//   D8 (byte12) = SEK   — sekundy, zwykle BCD (00..59)
//   D9 (byte13) = DISC  — numer plyty w STARSZYM nibblu (0x10=CD1 ... 0xA0=CD10)
//
// [HIGH-RISK] Ukladu pol ani markera 0x30 NIE zmieniac bez ponownego sniffu —
// to format zweryfikowany empirycznie wobec CDX-M670. Wczesniejsza wersja miala
// D5=0x00 (brak markera), liczbe utworow w D6 i plyte w D9 jako F|nr — przez co
// radio nie rozpoznawalo ramki jako czasu i mieszalo pola plyty/utworu.
//
// `prio` pozwala uzyc tej samej ramki jako natychmiastowego push (PRIO_STATUS).
// UWAGA: ramki czasu NIE sa juz pre-kolejkowane okresowo — buduje sie je SWIEZO
// w momencie grantu 0x13 (patrz handlePacket sekcja 5), dzieki czemu nie ma
// zalegania nieaktualnych ramek (miganie plyty/opoznienia) ani sztormu Slave
// Break (queued => break co 150ms => SYSTEM RESET).
static void buildStatusC0(uint8_t* frame) {
    uint8_t disc  = CdChanger::disk();
    uint8_t track = CdChanger::track();
    uint8_t min   = CdChanger::minutes();
    uint8_t sec   = CdChanger::seconds();

    uint8_t trkB  = UnilinkFrame::encodeBcdFpad(track);   // F-padded BCD
    uint8_t minB  = UnilinkFrame::encodeBcdFpad(min);     // F-padded BCD
    uint8_t secB  = UnilinkFrame::encodeBcd(sec);         // zwykle BCD
    // D9 = numer plyty w starszym nibblu + flaga stanu w mlodszym. Prawdziwa
    // zmieniarka w stanie Playing KONSEKWENTNIE ustawia bit3 (0x08) — w sniffie
    // CDX-M670 widziano np. CD8 -> 0x88, CD1 -> 0x18. Odwzorowujemy to 1:1 wobec
    // realnego sprzetu (UNILINK_PROTOKOL.md §6 podaje uproszczone 0x10=CD1 bez
    // flagi; gdyby radio nie pokazywalo plyty, sprobuj wlasnie 0x10|disc<<4).
    uint8_t discB = (uint8_t)(((disc & 0x0F) << 4) | 0x08);

    frame[0]  = 0x70;                      // RAD = display
    frame[1]  = myAddr;                    // TAD = nasz adres
    frame[2]  = 0xC0;                      // CMD1 = 0xC0 (status odtwarzania)
    frame[3]  = 0x00;                      // CMD2 = 0x00 (strona "czas", patrz §6)
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = 0x00;                      // D1 = rezerwa
    frame[6]  = 0x00;                      // D2 = rezerwa
    frame[7]  = 0x00;                      // D3 = rezerwa
    frame[8]  = 0x00;                      // D4 = rezerwa
    frame[9]  = 0x30;                      // D5 = MARKER strony odtwarzania
    frame[10] = trkB;                      // D6 = numer utworu (F-padded BCD)
    frame[11] = minB;                      // D7 = minuty (F-padded BCD)
    frame[12] = secB;                      // D8 = sekundy (BCD)
    frame[13] = discB;                     // D9 = numer plyty (starszy nibbel)
    frame[14] = UnilinkFrame::parity2(frame[4], &frame[5], 9);  // P2 nad D1..D9
    frame[15] = 0x00;                      // END
}

static void enqueueStatusC0(uint8_t prio) {
    uint8_t frame[16];
    buildStatusC0(frame);
    enqueue(prio, frame, sizeof(frame));
    if (DEBUG_VERBOSE) {
        Serial.printf(">> 0xC0 (status): CD%d TR%d %02d:%02d (prio=%d)\n",
                      CdChanger::disk(), CdChanger::track(),
                      CdChanger::minutes(), CdChanger::seconds(), prio);
    }
}

// Wyslij SWIEZA ramke 0xC0 natychmiast (uzywane na grant 0x13 gdy kolejka pusta).
static void sendFreshStatusC0() {
    uint8_t frame[16];
    buildStatusC0(frame);
    UnilinkBus::sendRaw(frame, sizeof(frame));
}

// ============================================================
// LEKKI TIK CZASU 0x90 (Kompendium §11.2) — format jak PRAWDZIWA zmieniarka
// ============================================================
// Realny CDX-805/CDX-M670 wysyla czas ramka 0x90 (middle, CMD2=0x30), a radio
// DOLICZA sekundy lokalnie miedzy odpytaniami (Kompendium §11.1). Dzieki temu
// czas plynie GLADKO mimo nieregularnego odpytywania. Wczesniej wysylalismy
// tylko 0xC0 (radio traktuje ja jak dyskretne odswiezenie, bez interpolacji) —
// stad "skakanie" czasu. Teraz w stanie Playing wysylamy 0x90 (gladki tik), a
// 0xC0 tylko gdy zmieni sie plyta/utwor (pelne odswiezenie z numerem plyty).
//
// Format z sniffu: 70 31 90 30 | P1 | F1 F0 03 8B | P2 | 00
//   D1 = utwor (BCD F-pad), D2 = minuty (BCD F-pad), D3 = sekundy (BCD),
//   D4 = bajt stanu (0x8X w stanie Playing).
static void buildLightTick0x90(uint8_t* frame) {
    uint8_t track = CdChanger::track();
    uint8_t min   = CdChanger::minutes();
    uint8_t sec   = CdChanger::seconds();

    frame[0] = 0x70;                                  // RAD = display
    frame[1] = myAddr;                                // TAD = nasz adres
    frame[2] = 0x90;                                  // CMD1 = 0x90 (middle, pozycja)
    frame[3] = 0x30;                                  // CMD2 = 0x30 (lekki tik)
    frame[4] = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5] = UnilinkFrame::encodeBcdFpad(track);    // D1 = utwor (F-pad)
    frame[6] = UnilinkFrame::encodeBcdFpad(min);      // D2 = minuty (F-pad)
    frame[7] = UnilinkFrame::encodeBcd(sec);          // D3 = sekundy (BCD)
    // D4: GORNY nibbel = numer plyty (jak D9 w 0xC0!), dolny = flaga "gra" (0x08).
    // Sniff CDX-M670 na CD8: D4=0x88/0x8A/0x8B = (8<<4)|flagi. Wczesniej bylo tu
    // 0x80 (gorny nibbel 8) -> radio pokazywalo "CD8" przy kazdym tiku 0x90 i
    // DISC migal 5<->8. Teraz spojnie z 0xC0: (disc<<4)|0x08.
    frame[8] = (uint8_t)(((CdChanger::disk() & 0x0F) << 4) | 0x08);  // D4 = plyta+flaga
    frame[9] = UnilinkFrame::parity2(frame[4], &frame[5], 4);  // P2 nad D1..D4
    frame[10] = 0x00;                                 // END
}

// Ostatnio nadana w pelnym statusie (0xC0) plyta/utwor — by wykryc zmiane i
// odswiezyc pelny status (z numerem plyty) zamiast samego tiku 0x90.
static uint8_t s_lastShownDisc  = 0;
static uint8_t s_lastShownTrack = 0;
static uint8_t s_tickCounter    = 0;

// Wybierz i NADAJ swiezy ekran na grant 0x13 przy pustej kolejce:
//   - poza Playing (ladowanie/zmiana/seek) -> pelny 0xC0 (numer plyty, "LOAD"),
//   - przy zmianie plyty/utworu -> raz pelny 0xC0 (odswiezenie numeru plyty),
//   - w ustalonym Playing -> przewaznie lekki tik 0x90 (gladki licznik, radio
//     interpoluje), a co kilka grantow pelny 0xC0. Prawdziwa zmieniarka tez
//     przeplata 0x90/0xC0 — to dodatkowo zabezpiecza aktualizacje czasu, gdyby
//     dane radio nie doliczalo sekund z samego 0x90.
static void sendFreshDisplay() {
    uint8_t disc  = CdChanger::disk();
    uint8_t track = CdChanger::track();
    bool playing  = (CdChanger::mechState() == CdChanger::MechState::Playing);

    bool changed = (disc != s_lastShownDisc || track != s_lastShownTrack);
    if (!playing || changed) {
        s_lastShownDisc  = disc;
        s_lastShownTrack = track;
        s_tickCounter    = 0;
        sendFreshStatusC0();           // pelny status z numerem plyty
        if (DEBUG_FRAMES) Serial.printf("   TX 0xC0 CD%d TR%d %02d:%02d\n",
                                        disc, track, CdChanger::minutes(), CdChanger::seconds());
        return;
    }

    // Ustalone Playing: co 4. grant pelny 0xC0, w pozostalych lekki tik 0x90.
    if (++s_tickCounter >= 4) {
        s_tickCounter = 0;
        sendFreshStatusC0();
        if (DEBUG_FRAMES) Serial.printf("   TX 0xC0 CD%d TR%d %02d:%02d\n",
                                        disc, track, CdChanger::minutes(), CdChanger::seconds());
    } else {
        uint8_t frame[11];
        buildLightTick0x90(frame);     // gladki tik czasu
        UnilinkBus::sendRaw(frame, sizeof(frame));
        if (DEBUG_FRAMES) Serial.printf("   TX 0x90 CD%d TR%d %02d:%02d\n",
                                        disc, track, CdChanger::minutes(), CdChanger::seconds());
    }
}

// Natychmiastowy push pelnego statusu (najwyzszy priorytet) — przy zmianie
// utworu/plyty/stanu (API publiczne, deklaracja w .h).
void enqueueFullStatusFrame() {
    enqueueStatusC0(Tx::PRIO_STATUS);
}

// Harmonogram ramki 0xC0 — ZACHOWANY dla zgodnosci API, ale CELOWO PUSTY.
// Pelny status jest budowany SWIEZO na grant 0x13 (sekcja 5 handlePacket),
// wiec nie ma potrzeby (ani sensu) pre-kolejkowac go okresowo — patrz komentarz
// przy servicePositionFrame1Hz (zaleganie ramek / sztorm Slave Break).
void serviceFullStatusFrame(unsigned long now) {
    (void)now;
    // celowo puste
}

// Natychmiastowe wysłanie pełnego statusu (poprawna ramka 0xC0 z markerem 0x30).
// Używana przy broadcastzie 0x08 (zakończenie przewijania) — R10.2.
void sendDisplayStatus() {
    enqueueFullStatusFrame();
}

} // namespace UnilinkProtocol
