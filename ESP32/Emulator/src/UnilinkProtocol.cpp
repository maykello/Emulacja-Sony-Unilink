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
// Czas ostatniego `01 12` bezposrednio do NAS (nie broadcast). Uzywane do
// rozroznienia "radio w housekeeping" (pinguje nas, ale nie robi 01 15) od
// "radio nas wyrzucilo" (brak jakiegokolwiek kontaktu). W fazie housekeeping
// radio przerywa Request Polling na ~2-3s, ale nadal pinguje urzadzenia.
static unsigned long lastPing12Ms = 0;

// --- MASKA ZGLOSZENIA DO ARBITRAZU (odpowiedz na `18 10 01 15`) ---
// Master odpytuje wszystkie urzadzenia jednoczesnie ramka `18 10 01 15`, a one
// ODPOWIADAJA ROWNOCZESNIE ramka `10 18 82 <maska>`. Magistrala jest typu
// wired-OR, wiec bajty wszystkich zgloszen sumuja sie bitowo — master widzi
// jedna ramke z maska wszystkich chetnych i po kolei rozdaje im granty `01 13`.
//
// Kazde urzadzenie dostaje swoj bit przy przydziale adresu: maska = dolny
// nibbel CMD2 ramki appoint. Potwierdzone sniffem CDX-M670:
//   3B 10 02 11  -> maska 0x01, w polls widac `82 01`, grant leci do 0x3B
//   31 10 02 14  -> maska 0x04, w polls widac `82 04`, grant leci do 0x31
//   oba chetne   -> `82 05` (0x01|0x04), master daje grant najpierw 0x3B, potem 0x31
//
// Wczesniej wysylalismy tu "typ ekranu" (0x01/0x04/0x05) zalezny od stanu
// mechanizmu. Przypadkiem trafialo to czasem w nasz bit, ale zwykle zglaszalo
// CUDZE urzadzenie — dlatego radio pytalo o ekran 0x3B zamiast nas, a my
// probowalismy wymusic swoje okno Slave Breakiem (i wywolywalismy kolizje).
constexpr uint8_t CLAIM_MASK_DEFAULT = 0x04;   // jak zmieniarka na 0x31
static uint8_t claimMask = CLAIM_MASK_DEFAULT;

// Czy mamy cokolwiek do nadania? (historyczne — patrz wantsBus: zawsze claim)
static bool wantsBus();

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
static unsigned long lastSystemResetMs   = 0;   // millis() ostatniego SYSTEM RESET (grace period)

// --- SLAVE BREAK / OCHRONA KOLIZJI ---
static unsigned long suppressBreakUntil = 0;
static unsigned long lastBreakTime      = 0;
// Czas ostatniego pobrania naszego ekranu przez radio (01 13).
static unsigned long lastDisplayServedMs = 0;
// Czas ostatniej ODDANEJ ramki ekranu.
static unsigned long lastDisplaySentMs = 0;
// Czas ostatniego `18 10 01 15` — Request Polling zywy ⇒ nie wolno Break.
static unsigned long lastPoll15Ms = 0;
// --- SESJA REQUEST POLLING (model burst OE) ---
// true = claim `82 <mask>`; false = `82 00` (konczymy burst jak OE po oddaniu ekranu).
static bool requestSessionActive = true;
static uint8_t sessionGrants = 0;
// Backoff gdy Hold OK, ale poll15 nie wraca.
static unsigned long breakBackoffMs = BREAK_RETRY_MS;
// Po udanym Hold czekamy na `01 15`; brak = zwieksz backoff przy kolejnym arm.
static unsigned long breakOkAwaitingPollMs = 0;

// --- CO RADIO MA TERAZ NA EKRANIE ---
// Plyta / utwor / bajt statusu z OSTATNIEJ ramki, ktora faktycznie oddalismy na
// grant `01 13`. Sluzy do dwoch rzeczy: wyboru miedzy pelnym 0xC0 a lekkim tikiem
// 0x90 (sendFreshDisplay) oraz do wykrycia, ze ekran radia jest nieaktualny i
// trzeba pilnie poprosic o magistrale (displayStale).
static uint8_t s_lastShownDisc  = 0;
static uint8_t s_lastShownTrack = 0;
static uint8_t s_lastShownState = 0xFF;
static uint8_t s_lastC0Min      = 0xFF;
static uint8_t s_lastC0Sec      = 0xFF;

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
static uint16_t statBreak = 0;    // ile breakow UZBROJONO
static uint16_t statBreakOk = 0;  // ile Hold ukonczono (wykrywalny break)
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
    // takeBreakCompleted() zjadane w serviceSlaveBreak (backoff / sesja).
    if (statPoll15 || statDisp13Us || statDisp13_3B || statPing12Us || statBtn ||
        statBreak || statBreakOk) {
        Serial.printf("[STAT] poll15=%u disp13(my)=%u disp13(3B)=%u ping12(my)=%u btn=%u break=%u/%u txt=%u seek=%u | alloc=%d state=0x%02X CD%d TR%d %02d:%02d\n",
                      statPoll15, statDisp13Us, statDisp13_3B, statPing12Us, statBtn,
                      statBreak, statBreakOk,
                      statText, statSeek,
                      deviceAllocated ? 1 : 0, statusByteFromState(CdChanger::mechState()),
                      CdChanger::disk(), CdChanger::track(),
                      CdChanger::minutes(), CdChanger::seconds());
    }
    statPoll15 = statDisp13Us = statDisp13_3B = statPing12Us = statBtn = 0;
    statBreak = statBreakOk = 0;
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
    resetCdTextCache();   // nowa sesja => nazwy trzeba wyslac od nowa
}

bool serviceTimeout(unsigned long now) {
    if (!deviceAllocated) return false;
    // Porownanie ZE ZNAKIEM. `now` moze byc minimalnie starsze od lastPingTime,
    // jesli wywolujacy pobral millis() przed obsluga ramek. Na typie bez znaku
    // taka roznica podwija sie do ~4 mld ms i timeout wywala sesje natychmiast
    // po przydzieleniu adresu — to byla przyczyna petli SYSTEM RESET.
    if ((long)(now - lastPingTime) <= (long)RADIO_TIMEOUT_MS) return false;

    // Radio zniklo — wracamy do stanu startowego (R4.1, R4.5).
    setAddrState(AddressManager::apply(addrState(), AddressManager::Event::Start, 0));
    claimMask = CLAIM_MASK_DEFAULT;   // wlasciwy bit nadejdzie z kolejnym appointem
    lastPingTime = now; // nie zglaszaj tego samego timeoutu raz za razem
    return true;
}

// Czy zglaszamy sie w arbitrazu `01 15`?
//
// [MODEL BURSTOWY] Zmieniarka zglasza sie tylko wtedy, gdy faktycznie ma co
// nadac, a po oproznieniu kolejki oddaje magistrale (`82 00`). Master konczy
// wtedy Request Polling i wraca do fali idle — to normalny cykl, potwierdzony
// sniffem prawdziwej zmieniarki (bursty 1-4 polli, przerwy 0.5-9.5 s) i opisem
// protokolu ("Request Polling: when slaves want to send data they make a
// request"). Nowy burst otwiera servicePositionFrame1Hz co sekunde, a jesli
// master zdazyl zamilknac — budzi go Slave Break (serviceSlaveBreak).
//
// Wariant "always-claim" (zglaszaj sie non stop w stanach aktywnych) probowal
// utrzymac Request Polling w nieskonczonosc. CDX-M670 i tak konczyl go po
// ~20 s, a poniewaz Slave Break byl wtedy nieskuteczny, polling juz nie wracal.
// Czy radio ma na ekranie NIEAKTUALNA plyte / utwor / stan mechanizmu?
//
// Tak jest zawsze zaraz po komendzie uzytkownika (track +/-, disc +/-, wybor
// 0xB0, start i koniec przewijania) oraz przy przejsciach LOAD -> Playing.
// Uzytkownik patrzy wtedy na wyswietlacz i czeka, wiec magistrali nie oddajemy
// i prosimy o nia od razu, zamiast czekac na zwykly rytm ~1 Hz. Bez tego numer
// plyty i licznik pojawialy sie z kilkusekundowym poslizgiem (audio ruszalo
// natychmiast, ekran dlugo pokazywal poprzednia plyte, a czas startowal od
// 00:05 zamiast 00:00).
static bool displayStale() {
    return CdChanger::disk()  != s_lastShownDisc  ||
           CdChanger::track() != s_lastShownTrack ||
           statusByteFromState(CdChanger::mechState()) != s_lastShownState;
}

static bool wantsBus() {
    if (!deviceAllocated) return false;
    if (!txQueue.isEmpty() || CdChanger::isDisplayDirty() || displayStale()) return true;
    return requestSessionActive;
}

static void openRequestSession() {
    requestSessionActive = true;
    sessionGrants = 0;
}

static void maybeCloseRequestSession() {
    // Sesji nie zamykamy, dopoki radio nie dostalo ramki EKRANU z biezacym
    // stanem. Sama kolejka nie wystarczy: grant obsluzony ramka 0x9C/0xD5
    // (zmiana plyty) nie odswieza wyswietlacza, wiec bez warunku displayStale
    // nowy numer plyty czekalby na kolejny tik 1 Hz.
    if (sessionGrants >= 1 && txQueue.isEmpty() &&
        !CdChanger::isDisplayDirty() && !displayStale()) {
        requestSessionActive = false;
    }
}

void serviceSlaveBreak(bool busPowered) {
    if (!busPowered || !deviceAllocated) {
        UnilinkBus::cancelSlaveBreak();
        return;
    }

    UnilinkBus::serviceSlaveBreak();

    const unsigned long nowMs = millis();

    // Po udanym Hold — otworz sesje claim; na `01 15` czekamy w POLL15 handlerze.
    const uint16_t justDone = UnilinkBus::takeBreakCompleted();
    if (justDone) {
        statBreakOk += justDone;
        openRequestSession();
        breakOkAwaitingPollMs = nowMs;
    }

    if (UnilinkBus::slaveBreakPending()) return;

    // Break uzbrajamy, gdy mamy co nadac, a Request Polling stoi. To jedyny
    // przewidziany przez protokol sposob, w jaki slave prosi o magistrale.

    // [GRACE PERIOD] Tuz po SYSTEM RESET radio robi discovery — w tym czasie
    // naturalnie nie ma `01 15`. Nie uzbrajaj Break, bo wyzwoli kolizje.
    if ((nowMs - lastSystemResetMs) < POST_RESET_GRACE_MS) return;

    // Polling zywy — nie potrzebujemy Break.
    //
    // Nie ma tu juz warunku na Time Poll (`01 12`). Radio pinguje nim co ~600 ms
    // NIEZALEZNIE od Request Pollingu, wiec warunek "nie rob Break, dopoki radio
    // nas pinguje" blokowal Break na zawsze: po zamknieciu `01 15` emulator
    // milczal, a ekran zastygal na stale (log 20:31: poll15=0, break=0/0,
    // ping12=1 co 2 s, czas na ekranie stoi mimo dzialajacego licznika).
    if ((nowMs - lastPoll15Ms) < POLL15_QUIET_BREAK_MS) {
        breakBackoffMs = BREAK_RETRY_MS;
        breakOkAwaitingPollMs = 0;
        return;
    }

    // Plaski retry zamiast eksponencjalnego backoffu.
    if (breakOkAwaitingPollMs != 0 &&
        (nowMs - breakOkAwaitingPollMs) >= BREAK_RECOVERY_MS) {
        breakOkAwaitingPollMs = 0;
    }

    // Ekran radia rozjechany ze stanem zmieniarki => prosba PILNA: pomijamy
    // okno po poprzednim Breaku i skracamy odstep do BREAK_URGENT_MIN_MS.
    // Okno kolizyjne po odpytaniu obcego urzadzenia (suppressBreakUntil)
    // zostaje — ono chroni cudza odpowiedz, nie nasz rytm.
    const bool urgent = displayStale();
    if (!urgent && UnilinkBus::breakRecoveryActive(nowMs)) return;
    if (nowMs < suppressBreakUntil) return;
    if (nowMs - lastBreakTime < (urgent ? BREAK_URGENT_MIN_MS : breakBackoffMs)) return;
    if (!wantsBus()) return;

    // Uzbrojenie Breaka jest teraz RUTYNA (raz na ~sekunde, gdy master spi), a
    // nie objawem awarii — stad brak zrzutu czarnej skrzynki i logu na kazde
    // wystapienie. Zrzut 128 ramek co 10 s blokowal petle na tyle, ze sam
    // wywolywal kolizje. Licznik `break=N/M` widac w [STAT].
    UnilinkBus::requestSlaveBreak();
    lastBreakTime = nowMs;
    statBreak++;
    if (DEBUG_FRAMES) {
        Serial.printf("   BREAK armed (grant %lums ago, poll15 %lums ago)\n",
                      nowMs - lastDisplayServedMs, nowMs - lastPoll15Ms);
    }
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
// Format ramki 0xD2 (long, 16B) odczytany z tych ramek:
//   "Kapita"  -> CMD2='K', D1..D5="apita", D6=0x02, D7=0x00, D8=0xF1, D9=0x10
//   "nskie t" -> CMD2='n', D1..D6="skie t",         D7=0x01, D8=0xF1, D9=0x10
// czyli tekst leje sie przez sloty CMD2,D1..D6, D6=0x02 znaczy "ciag dalszy",
// a D7=0x01 znaczy "to ostatnia ramka nazwy". D8 = numer UTWORU jako F|nr
// (0xF1..0xF4 dla utworow 1..4 w sniffie), D9 = 0x10. Zaraz po nazwie utworu
// zmieniarka wysyla ramke 0xDA z nazwa plyty (D8 = numer plyty).
//
// Wczesniej wpisywalismy w D7 numer pola, a w D8 numer plyty i wysylalismy po
// jednej ramce na zadanie — radio nie mialo jak rozpoznac konca nazwy i nie
// skladalo tekstu.
//
// [WARIANT_PROTOKOLU §10.3] To jest format uzywany przez CDX-M670. Starszy
// handler 0xC9/0xD9 (8 znakow) pozostaje dla radii uzywajacych wariantu §10.2.
// Maksymalna dlugosc nazwy przesylanej w tym wariancie. Prawdziwa zmieniarka
// obcina KAZDA nazwe do 13 znakow (wszystkie zaobserwowane nazwy: "Kapitanskie t",
// "Do zakochania", "Staruszek Swi" — dokladnie 13 znakow, dwie ramki 6+7).
constexpr int D2_MAX_CHARS  = 13;
constexpr int D2_SLOT_COUNT = 8;   // CMD2 + D1..D7

// Zbuduj 8 slotow jednego segmentu nazwy (CMD2, D1..D7) wg ukladu ze sniffu:
//   segment NIEOSTATNI : 6 znakow w slotach 0..5, slot6 = 0x02 ("ciag dalszy"),
//                        slot7 = 0x00
//   segment OSTATNI    : do 7 znakow w slotach 0..6 (reszta 0x00),
//                        slot7 = 0x01 ("koniec nazwy")
// Zwraca liczbe znakow zuzytych z `name`.
static int buildTextSegment(const char* name, int offset, bool last, uint8_t* slots) {
    for (int i = 0; i < D2_SLOT_COUNT; ++i) slots[i] = 0x00;
    const int capacity = last ? 7 : 6;
    int used = 0;
    while (used < capacity && name[offset + used] != '\0') {
        slots[used] = (uint8_t)name[offset + used];
        used++;
    }
    slots[6] = last ? slots[6] : 0x02;   // marker kontynuacji tuz za tekstem
    slots[7] = last ? 0x01 : 0x00;       // 0x01 = ostatnia ramka nazwy
    return used;
}

// Zloz i zakolejkuj jedna ramke nazwy (0xD2 = utwor, 0xDA = plyta).
// Uklad long (16B): CMD2 + D1..D5 = znaki, D6/D7 = markery, D8 = numer utworu
// F|nr (0xD2) albo numer plyty (0xDA), D9 = 0x10.
static void enqueueTextFrame(uint8_t cmd1, const uint8_t* slots, uint8_t d8) {
    uint8_t frame[16];
    frame[0]  = 0x70;                      // RAD = wyswietlacz
    frame[1]  = myAddr;                    // TAD = nasz adres
    frame[2]  = cmd1;                      // CMD1 = 0xD2 (utwor) / 0xDA (plyta)
    frame[3]  = slots[0];                  // CMD2 = znak 0
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    for (int i = 1; i < D2_SLOT_COUNT; ++i) {
        frame[4 + i] = slots[i];           // D1..D7 = znaki 1..5 + markery
    }
    frame[12] = d8;                        // D8 = numer utworu / plyty
    frame[13] = 0x10;                      // D9 (stale w sniffie CDX-M670)
    frame[14] = UnilinkFrame::parity2(frame[4], &frame[5], 9);
    frame[15] = 0x00;                      // END
    enqueue(Tx::PRIO_CD_TEXT, frame, sizeof(frame));
}

// Zakolejkuj CALA nazwe (wszystkie segmenty) jednym ciagiem. Prawdziwa
// zmieniarka odpowiada na kolejne granty dokladnie tak: segment 0, segment 1,
// a potem ramka nazwy plyty 0xDA.
static void enqueueTextName(uint8_t cmd1, const char* rawName, uint8_t d8) {
    char sane[64];
    CdText::sanitizeAscii(rawName, sane, sizeof(sane));
    int total = 0;
    while (sane[total] != '\0' && total < D2_MAX_CHARS) total++;
    sane[total] = '\0';

    uint8_t slots[D2_SLOT_COUNT];
    if (total == 0) {
        // Pusta nazwa: zmieniarka i tak wysyla jedna ramke-placeholder z
        // markerem kontynuacji (sniff: `70 31 DA 00 7B 00 00 00 00 00 02 00 ...`).
        buildTextSegment(sane, 0, /*last=*/false, slots);
        enqueueTextFrame(cmd1, slots, d8);
        return;
    }

    int offset = 0;
    while (offset < total) {
        const bool last = (total - offset) <= 7;
        offset += buildTextSegment(sane, offset, last, slots);
        enqueueTextFrame(cmd1, slots, d8);
    }
}

// Odpowiedz na `84 D7`: nazwa biezacego utworu (0xD2), a po niej nazwa plyty
// (0xDA) — dokladnie taka sekwencje wysyla prawdziwa zmieniarka.
static void enqueueCdTextD2Track() {
    const uint8_t disc  = CdChanger::disk();
    const uint8_t track = CdChanger::track();

    // Nie duplikuj nazw, gdy poprzedni komplet jeszcze nie zszedl z kolejki.
    if (!txQueue.isEmpty()) return;

    char raw[64];
    audioGetTrackName(disc, track, raw, sizeof(raw));
    enqueueTextName(0xD2, raw, UnilinkFrame::encodeDiscNibble(track));

    audioGetDiscName(disc, raw, sizeof(raw));
    enqueueTextName(0xDA, raw, (uint8_t)(disc & 0x0F));
}

// ------------------------------------------------------------
// MAGAZYNEK: ADRESOWANIE ODPOWIEDZI (RAD)
// ------------------------------------------------------------
// Ramki opisujace TRESC EKRANU (0x95 mapa magazynka, 0x97 info plyty, 0x94
// ikony, 0xC0 status, nazwy CD-TEXT) ida na RAD=0x70 — tak jak w sniffie
// (`70 31 95 ...`, `70 31 8E ...`, `70 31 C0 ...`). Wyjatkiem sa ramki
// identyfikatora plyty 0xC5/0xD5, ktore prawdziwa zmieniarka adresuje do
// mastera (`10 31 C5 ...`) — obsluguje je enqueueDiscId, nie ten helper.
// TAD=myAddr we wszystkich odpowiedziach.
static constexpr uint8_t MAGAZINE_RAD = 0x70;  // RAD = wyswietlacz (jak 0xC0 / CD-TEXT)

// ------------------------------------------------------------
// Magazynek: zbuduj i ZAKOLEJKUJ ramke middle (0x95 mapa / 0x97 info plyty).
// data4 = 4 bajty danych (D1..D4). CMD1 0x80..0xBF => ramka middle (11B).
// ------------------------------------------------------------
static void enqueueMagazineMiddle(uint8_t cmd1, uint8_t cmd2, const uint8_t* data4) {
    uint8_t frame[11];
    frame[0]  = MAGAZINE_RAD;          // RAD = wyswietlacz
    frame[1]  = myAddr;                // TAD = nasz adres
    frame[2]  = cmd1;                  // CMD1 = 0x95 / 0x97 (middle)
    frame[3]  = cmd2;                  // CMD2
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
// Magazynek: zbuduj i ZAKOLEJKUJ ramke disc ID 0xC5/0xD5 (long, 9B danych).
// ------------------------------------------------------------
// Wywolywane przy skanie magazynka (0xC5) i przy zmianie plyty (0xD5) — R7.3.
// Disc ID jest staly dla danej plyty miedzy zadaniami (R7.5); cache'uje go
// Magazine::buildDiscId. CMD1 >= 0xC0 => ramka long (16B).
void enqueueDiscId(uint8_t disc, bool discChangeVariant) {
    uint8_t d[Magazine::DISC_ID_DATA_LEN];
    uint8_t cmd2 = 0x00;
    Magazine::buildDiscId(disc, discChangeVariant, cmd2, d);

    uint8_t frame[16];
    // RAD = 0x10 (master), NIE 0x70. Sniff CDX-M670:
    //   10 31 C5 A2 A8 | 24 77 52 F0 00 00 00 00 88 | 0D 00   (skan magazynka)
    //   10 31 D5 A2 B8 | 24 77 52 F1 00 00 00 00 88 | 1E 00   (zmiana plyty)
    // Identyfikator plyty to dane dla logiki radia (Custom File), a nie tresc
    // ekranu — dlatego trafia do mastera. Wczesniej wysylalismy go na 0x70.
    frame[0]  = ADDR_MASTER;           // RAD = master
    frame[1]  = myAddr;                // TAD = nasz adres
    frame[2]  = discChangeVariant ? 0xD5 : 0xC5;
    frame[3]  = cmd2;                  // CMD2 = 0xA|setne sekundy TOC
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    for (int i = 0; i < Magazine::DISC_ID_DATA_LEN; ++i) {
        frame[5 + i] = d[i];           // D1..D9 = TOC plyty + numer plyty
    }
    frame[14] = UnilinkFrame::parity2(frame[4], &frame[5], Magazine::DISC_ID_DATA_LEN);  // P2 nad D1..D9
    frame[15] = 0x00;                  // END
    enqueue(Tx::PRIO_DISC_ID, frame, sizeof(frame));
}

// ------------------------------------------------------------
// ZMIANA PLYTY: ramka 0x9C (middle) — powiadomienie dla radia.
// ------------------------------------------------------------
// Sniff CDX-M670: `90 31 9C 00 | 5D | 00 00 00 10 | 6D | 00` (CD1) oraz
// `90 31 9C 00 5D 00 00 00 88 E5 00` (CD8). RAD=0x90, CMD2=0x00, D1..D3=0,
// D4 = numer plyty w gornym nibblu. Emulator wczesniej w ogole nie wysylal tej
// ramki, przez co radio nie odswiezalo numeru plyty po `0x28`/`0x29`/`0xB0`.
static void enqueueDiscChange(uint8_t disc) {
    uint8_t frame[11];
    frame[0]  = 0x90;                  // RAD = 0x90 (wg sniffu)
    frame[1]  = myAddr;                // TAD = nasz adres
    frame[2]  = 0x9C;                  // CMD1 = 0x9C (middle, zmiana plyty)
    frame[3]  = 0x00;                  // CMD2
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = 0x00;                  // D1
    frame[6]  = 0x00;                  // D2
    frame[7]  = 0x00;                  // D3
    frame[8]  = UnilinkFrame::discHighNibble(disc, 0x00);   // D4 = numer plyty
    frame[9]  = UnilinkFrame::parity2(frame[4], &frame[5], 4);
    frame[10] = 0x00;                  // END
    enqueue(Tx::PRIO_STATUS, frame, sizeof(frame));
}

// Wspolna reakcja na KAZDA zmiane plyty (0x28 / 0x29 / 0xB0 / auto-advance):
// powiadomienie 0x9C + identyfikator plyty 0xD5 (R7.3).
static void onDiscChanged(uint8_t disc) {
    enqueueDiscChange(disc);
    enqueueDiscId(disc, /*discChangeVariant=*/true);
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
// Buduje lekki tik 0x90 (definicja nizej). Uzywana przez servicePositionFrame1Hz.
static void buildLightTick0x90(uint8_t* frame);
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

// [NAPRAWA ZASTYGANIA] Harmonogram 1Hz — kolejkuje ramke czasu co sekunde w
// stanie Playing. Prawdziwa zmieniarka ZAWSZE ma cos do oddania na grant
// `01 13`. Gdy kolejka jest pusta i nie kolejkujemy ramek czasu, wantsBus()
// w stanach nieaktywnych zwraca false -> emulator milczy -> ekran zastyga.
// Nawet w nowym modelu "always claim" kolejka z ramka czasu gwarantuje, ze na
// grant 0x13 mamy SWIEZA ramke z aktualnym czasem (zamiast polegac wylacznie
// na sendFreshDisplay, ktory buduje ramke w ostatniej chwili).
//
// Zabezpieczenie przed zaleganiem: kolejkujemy MAX JEDNA ramke czasu naraz
// (sprawdzamy czy kolejka ma wolne miejsce i czy minela co najmniej sekunda).
void servicePositionFrame1Hz(unsigned long now) {
    if (CdChanger::mechState() != CdChanger::MechState::Playing) {
        lastPositionUpdateMs = now;  // reset, zeby nie kolejkowac zaleglosci
        return;
    }
    if ((now - lastPositionUpdateMs) < 1000) return;
    lastPositionUpdateMs = now;

    // [MODEL BURSTOWY] Co sekunde otwieramy nowa sesje claim — to jedyny
    // mechanizm wyzwalajacy burst. Bez tego emulator milczalby po zamknieciu
    // sesji (claim `82 00`) i radio nie dostaloby aktualizacji czasu.
    //
    // Samej ramki czasu tu NIE kolejkujemy: burst potrafi ruszyc dopiero po
    // Slave Breaku, wiec pre-kolejkowany tik dojechalby do radia z nieaktualna
    // sekunda. Ramke buduje sendFreshDisplay dokladnie w chwili grantu `01 13`.
    openRequestSession();
}

// ============================================================
// CD-TEXT NADAWANY SAM Z SIEBIE (bez zadania radia)
// ============================================================
// Zrzut prawdziwej zmieniarki na CDX-M670 zawiera ~43 ramki nazw (0xD2 utwor /
// 0xDA plyta), a tylko 2 zadania `84 D7`. Zmieniarka NIE czeka wiec, az radio
// poprosi — po kazdej zmianie utworu sama wypycha komplet nazw na najblizszych
// grantach. Nasz emulator odpowiadal wylacznie na zadania, a CDX-M670 ich nie
// wysylal (licznik `txt=0` w kazdym [STAT]) — stad pusty CD-TEXT.
static uint8_t s_textSentDisc  = 0;
static uint8_t s_textSentTrack = 0;

void resetCdTextCache() {
    s_textSentDisc  = 0;
    s_textSentTrack = 0;
}

void serviceCdText(unsigned long) {
    if (!deviceAllocated) return;
    const CdChanger::MechState ms = CdChanger::mechState();
    if (ms == CdChanger::MechState::Init || ms == CdChanger::MechState::Idle) return;

    const uint8_t disc  = CdChanger::disk();
    const uint8_t track = CdChanger::track();
    if (disc == s_textSentDisc && track == s_textSentTrack) return;

    // Nazwy sa najmniej pilne ze wszystkiego — wchodza dopiero, gdy kolejka jest
    // pusta, zeby nie opozniac statusu i pozycji. Znacznik aktualizujemy DOPIERO
    // po zakolejkowaniu, wiec przy zajetej kolejce sprobujemy w kolejnej iteracji.
    if (!txQueue.isEmpty()) return;

    enqueueCdTextD2Track();
    s_textSentDisc  = disc;
    s_textSentTrack = track;
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

    // ===== FILTR ECHA (zabezpieczenie magistrali wired-OR) =====
    // Ramka, w ktorej TAD == myAddr, to NASZA odpowiedz odczytana z powrotem
    // z magistrali wired-OR. Prawdziwa ramka OD mastera DO nas ma zawsze
    // RAD == myAddr (adresowana do nas), a TAD == 0x10/0x14 (master/display).
    // Nasze odpowiedzi maja TAD == myAddr (my nadajemy). Jesli taka ramka
    // przejdzie flush ISR (Fix 1) i walidacje parity, to jest echem — odrzucamy.
    // Bez tego echa zaburzaja dyspozytor: lastPingTime jest falszywie
    // odswiezany, a widmowe ramki konsumuja sloty w buforze RX.
    if (deviceAllocated && tad == myAddr) {
        Diagnostics::recordNote("ECHO");
        return;
    }

    // ===== Diagnostyka: zliczanie kluczowych ramek =====
    if (rad == ADDR_BROADCAST && op1 == 0x01 && op2 == 0x15) {
        statPoll15++;
        lastPoll15Ms = millis();
        breakBackoffMs = BREAK_RETRY_MS;  // master prowadzi arbitraz
        if (UnilinkBus::slaveBreakPending()) {
            UnilinkBus::cancelSlaveBreak();
        }
    }
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

    // ===== PING NATYCHMIASTOWY (01 12) — BEZWARUNKOWY handler =====
    // Time Poll 01 12 od mastera (0x10) lub procesora ekranu (0x14) wymaga
    // NATYCHMIASTOWEJ odpowiedzi. Brak odpowiedzi = radio uznaje zmieniarke
    // za nieosiagalna i robi SYSTEM RESET / usuwa z sesji.
    // KLUCZOWE: CDX-M670 odpytuje status ROWNIEZ z procesora ekranu
    // (tad=0x14): "31 14 01 12". Brak odpowiedzi na nie powodowal SYSTEM RESET.
    // Handler jest BEZWARUNKOWY (if, nie else-if), przed calym lancuchem
    // else-if, aby NIGDY nie byl blokowany przez inne galezie dyspozytora.
    if (rad == myAddr && (tad == ADDR_MASTER || tad == ADDR_DISPLAY) &&
        op1 == 0x01 && op2 == 0x12) {
        if (tad == ADDR_MASTER) CdChanger::noteFirstPing();
        CdChanger::notePolled();
        lastPing12Ms = millis();  // radio nadal nas widzi — nie robimy auto-recovery
        UnilinkBus::sendShort(tad, myAddr, 0x00, statusByteFromState(CdChanger::mechState()));
        if (DEBUG_VERBOSE) {
            Serial.printf(">> PING odpowiedz (do 0x%02X): status=0x%02X\n",
                          tad, statusByteFromState(CdChanger::mechState()));
        }
        return;  // obsluzono — nie wchodzi w lancuch else-if
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

    // ===== 1a. UNAPPOINTED CHANGER QUERY (18 10 01 11) =====
    // Prawdziwa zmieniarka odpowiada magicznym pakietem `10 18 04 00 2C 00`,
    // po ktorym master robi SYSTEM RESET i uruchamia wlasciwe discovery. Sniff:
    //   18 10 01 11 3A 00
    //   10 18 04 00 2C 00     <- zmieniarka
    //   18 10 01 00 29 00     <- master: system reset -> discovery
    // Po przydzieleniu adresu zmieniarka MILCZY na `01 11` (w sniffie kilkadziesiat
    // takich odpytan bez odpowiedzi) — stad warunek `!deviceAllocated`.
    //
    // Wczesniej odpowiadalismy magic-pakietem takze na `01 01` (w trybie
    // CDX-M670). Prawdziwa zmieniarka NIGDY tego nie robi — `01 01` przechodzi u
    // niej bez echa. Nasza dodatkowa odpowiedz wywolywala u radia zbedny SYSTEM
    // RESET w srodku discovery, czyli dokladnie te petle resetow, ktora mielismy
    // naprawiac.
    else if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x11) {
        // [FIX 4 - AUTO-RECOVERY] Jesli radio pyta "jest tu nieprzydzielona
        // zmieniarka?" (01 11), a my od dawna nie widzielismy Request Polling
        // (01 15), to znaczy ze radio nas wyrzucilo z sesji (np. po nieudanym
        // Time Poll 01 12). Resetujemy stan i odpowiadamy magic, zeby radio
        // zainicjowalo nowe discovery i ponownie nas przydzielilo.
        //
        // [GRACE PERIOD] Po SYSTEM RESET radio robi discovery, w ktorym
        // naturalnie NIE MA `01 15`. Bez grace period emulator interpretowal
        // to jako "poll15 martwy" i wyzwalal auto-recovery = kolejny reset.
        const unsigned long nowAR = millis();
        const bool graceExpired = (nowAR - lastSystemResetMs) > POST_RESET_GRACE_MS;
        // [HOUSEKEEPING] Radio w fazie housekeeping pinguje nas 01 12, ale NIE
        // robi 01 15. To NORMALNY cykl (~2-3s co ~15s). Nie robimy auto-recovery
        // dopoki radio nas widzi (odpytuje 01 12). Auto-recovery TYLKO gdy radio
        // naprawde przestalo sie z nami komunikowac.
        const bool radioStillPingsUs = (nowAR - lastPing12Ms) < RADIO_TIMEOUT_MS;
        if (deviceAllocated && graceExpired && !radioStillPingsUs &&
            (nowAR - lastPoll15Ms) > POLL15_ALIVE_MS) {
            Serial.println(">> 01 11 a poll15 nie wraca — reset sesji, ponowne discovery");
            Diagnostics::dump("AUTO-RECOVERY: 01 11 + dead poll15");
            setAddrState(AddressManager::apply(addrState(), AddressManager::Event::Start, 0));
            claimMask = CLAIM_MASK_DEFAULT;
            CdChanger::resetToAllocated();   // zachowaj audio jesli gra
            CdChanger::clearDisplayDirty();
            txQueue.clear();  // wyczysc stara kolejke — nowa sesja
            suppressBreakUntil = 0;
            lastBreakTime = 0;
            breakBackoffMs = BREAK_RETRY_MS;
            breakOkAwaitingPollMs = 0;
        }
        if (!deviceAllocated) {
            const uint8_t magic[] = {0x10, 0x18, 0x04, 0x00, 0x2C, 0x00};
            UnilinkBus::sendRaw(magic, sizeof(magic));
            Serial.println(">> Odpowiadam na 01 11: magic 10 18 04 00 -> nowe discovery");
        }
    }

    // ===== 1b. SYSTEM RESET (18 10 01 00) =====
    // Radio przerywa sesje i zaczyna discovery od nowa. Zapominamy adres.
    else if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x00) {
        // Zrzuc czarna skrzynke ZANIM zresetujemy stan — pokaze ramki, ktore
        // doprowadzily do resetu radia.
        Diagnostics::dump("RADIO SYSTEM RESET 18 10 01 00");
        resetLoopCount++;
        lastSystemResetMs = millis();  // grace period dla auto-recovery i Slave Break
        if (deviceAllocated) {
            Serial.printf(">> Radio system reset (#%d)! Reset deviceAllocated.\n", resetLoopCount);
            // [NAPRAWA CZASU] Uzywamy resetToAllocated() zamiast resetToInit():
            // jezeli audio nadal gra, zachowujemy stan Playing i czas. Prawdziwa
            // zmieniarka po SYSTEM RESET wraca z zachowanym stanem odtwarzania.
            CdChanger::resetToAllocated();
            CdChanger::clearDisplayDirty();
        } else {
            Serial.printf(">> Radio system reset (#%d) (juz bylem nieprzydzielony)\n", resetLoopCount);
        }
        // Bus reset -> {0x30, false} (R4.4). Nastepny przydzial moze byc inny.
        setAddrState(AddressManager::apply(addrState(), AddressManager::Event::BusReset, rad));
        lastPreliminaryTime = 0;
        anyoneIgnoredCount = 0;
        txQueue.clear();  // stara kolejka nieaktualna po resecie sesji
        resetCdTextCache();
    }

    // ===== 2. ADDRESS APPOINT (3X 10 02 XX) =====
    // Radio przydziela adres z grupy CD. Adoptujemy DOWOLNE ID z grupy
    // (RAD & 0xF0)==0x30, czyli 0x30..0x3F — nie tylko 0x31..0x3A (R4.3,
    // Kompendium §6.2). op2 i sam adres roznia sie miedzy radiami.
    else if (tad == ADDR_MASTER && op1 == 0x02 && AddressManager::isCdGroup(rad)) {
        setAddrState(AddressManager::apply(addrState(), AddressManager::Event::Appoint, rad));
        // Wraz z adresem master przydziela nam bit w arbitrazu `01 15` — jest to
        // dolny nibbel CMD2 tej ramki (0x14 -> 0x04 w sniffie CDX-M670).
        if ((op2 & 0x0F) != 0) {
            claimMask = (uint8_t)(op2 & 0x0F);
        }
        // Okno "master o mnie zapomnial" liczymy od przydzialu adresu. Bez tego
        // lastDisplayServedMs zostaje na 0 i tuz po appoincie wygladamy na
        // zaglodzonych — emulator wystawilby Slave Break w srodku discovery.
        lastDisplayServedMs = millis();
        lastDisplaySentMs   = lastDisplayServedMs;
        lastPoll15Ms        = lastDisplayServedMs;
        lastPing12Ms        = lastDisplayServedMs;  // nie wyzwalaj auto-recovery tuz po appoint
        openRequestSession();
        breakBackoffMs = BREAK_RETRY_MS;
        resetCdTextCache();   // radio zaczyna od pustego ekranu — nazwy lecą ponownie
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

    // ===== 3. ARBITRAZ — kto chce nadawac? (18 10 01 15) =====
    // Odpowiedz: 10 18 82 <maska> | PAR1 | 00 00 00 00 | PAR2 00.
    // <maska> to NASZ bit przydzielony przy appoint (patrz claimMask), a nie
    // "typ ekranu" — wszystkie urzadzenia odpowiadaja rownoczesnie, a wired-OR
    // skleja ich bity w jedna ramke, ktora master rozklada na kolejne granty.
    else if (rad == ADDR_BROADCAST && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x15) {
        // Dopoki nie mamy adresu, nie mamy tez przydzielonego bitu — milczymy,
        // dokladnie tak jak prawdziwa zmieniarka przed appointem.
        if (!deviceAllocated) return;
        // [HEARTBEAT] Radio WYMAGA widziec odpowiedz na 01 15 od urzadzen
        // na magistrali. Bez niej (nawet gdy 82 00) radio przestaje pollowac
        // i ekran zmieniarki nie wyswietla sie. Dlatego ZAWSZE nadajemy:
        // `82 <claimMask>` gdy chcemy bus, `82 00` gdy nie.
        const uint8_t mask = wantsBus() ? claimMask : 0x00;
        UnilinkBus::sendMedium(0x10, 0x18, 0x82, mask, 0x00, 0x00, 0x00, 0x00);
        if (DEBUG_VERBOSE) {
            Serial.printf(">> 01 15 (arbitraz): zglaszam maske 0x%02X (status=0x%02X)\n",
                          mask, statusByteFromState(CdChanger::mechState()));
        }
    }

    // ===== 4. PING (01 12) — OBSLUZONY BEZWARUNKOWO NA SZCZYCIE DYSPOZYTORA =====
    // Handler przeniesiony przed lancuch else-if, aby NIGDY nie byl blokowany
    // przez inne galezie. Patrz sekcja "PING NATYCHMIASTOWY" powyzej.

    // ===== 5. UPDATE DISPLAY (3X 10 01 13) =====
    // TYLKO gdy tad == 0x10 (pytanie do nas). NIE odpowiadamy na 31 14 01 13 —
    // to pytanie do display processora (0x14), nie do nas.
    // Grant po arbitrazu `01 15`: zdejmujemy DOKLADNIE JEDNA ramke z kolejki TX
    // i ja nadajemy. Gdy kolejka jest pusta, budujemy swiezy ekran w miejscu —
    // tak jak prawdziwa zmieniarka, ktora na kazdy grant odpowiada aktualnym
    // stanem (0x90 tik czasu / 0xC0 pelny status / 0x8E ekran spoczynku).
    else if (rad == myAddr && tad == ADDR_MASTER && op1 == 0x01 && op2 == 0x13) {
        lastDisplayServedMs = millis();
        lastDisplaySentMs   = lastDisplayServedMs;
        sessionGrants++;
        CdChanger::notePolled();
        Tx::TxItem item;
        if (txQueue.dequeue(item)) {
            UnilinkBus::sendRaw(item.bytes, item.len);
        } else {
            sendFreshDisplay();
        }
        // Ekran oddany — kasujemy flage w OBU sciezkach. Gdyby kasowal ja tylko
        // sendFreshDisplay, kazdy grant obsluzony z kolejki zostawialby dirty=1,
        // sesja claim nigdy by sie nie zamknela i model burstowy zamienilby sie
        // z powrotem w always-claim.
        CdChanger::clearDisplayDirty();
        // Zamknij burst — kolejne `01 15` dostana `82 00` (krotkie serie OE).
        maybeCloseRequestSession();
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

    // ===== 6a-alt. KEY OFF (18 10 08 00) — PUSZCZENIE klawisza =====
    // Kompendium §9. Dla FF/REW to koniec przewijania: razem z ramka 0x24/0x25
    // (wcisniecie) wyznacza czas trzymania klawisza, z ktorego CdChanger liczy
    // przebyty dystans (SCAN_RATE* w Config.h). Po zatrzymaniu skanu mechanizm
    // wraca do Playing z osiagnieta pozycja.
    // Bezpiecznik na wypadek zgubionego `08 00`: SEEK_SCAN_MAX_MS.
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
        onDiscChanged(CdChanger::disk());   // R7.3: 0x9C + disc ID
    }
    else if (rad == myAddr && op1 == 0x29) {
        CdChanger::prevDisc();
        onDiscChanged(CdChanger::disk());   // R7.3: 0x9C + disc ID
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
    // Radio prosi o mape obecnosci plyt (R7.1). Odpowiedz kolejkowana
    // (Tx::PRIO_MAGAZINE), nadawana po grancie 0x13.
    else if (rad == myAddr && op1 == 0x84 && op2 == 0x95) {
        // Sony NIE uklada bitow obecnosci po kolei. Sniff CDX-M670 z jedna
        // plyta (CD8) w magazynku: `70 31 95 08 3E 00 00 00 8A C8 00` — bit3
        // CMD2 = CD8, D1..D3 = 0, D4 = numer plyty w gornym nibblu | 0x0A.
        // Wczesniej wysylalismy zwykla mape liniowa w D1/D2, wiec radio
        // pokazywalo pusty magazynek.
        const uint16_t pm = Magazine::presenceMap();
        uint8_t data[4] = {
            Magazine::magazineD1FromMap(pm),                                  // D1 = CD9/CD10
            0x00,                                                             // D2
            0x00,                                                             // D3
            UnilinkFrame::discHighNibble(CdChanger::disk(), 0x0A)             // D4 = biezaca plyta
        };
        enqueueMagazineMiddle(0x95, Magazine::magazineCmd2FromMap(pm), data);
    }

    // ===== 10b. Bezposredni wybor płyty (0xB0) — Wymaganie 9 =====
    // Format: RAD TAD 0xB0 disc track (short frame, 6B). CMD2=disc, D1=track.
    // Weryfikacja zakresu przez Magazine/AudioPlayer. Przy poprawnym zakresie
    // ustawia biezaca płyte/utwór, wejdz w SEEK i zakolejkuj zaktualizowany status.
    // Poza zakresem zignoruj bez zmiany stanu.
    else if (rad == myAddr && op1 == 0xB0) {
        // 0xB0 lezy w 0x80..0xBF => ramka MIDDLE (11B): D1 to buf[5], a NIE
        // buf[4] (buf[4] to Parity1!). Numer plyty siedzi w dolnym nibblu CMD2.
        uint8_t disc  = (uint8_t)(op2 & 0x0F);
        uint8_t track = 1;
        if (len >= 11) {
            const uint8_t raw = buf[5];
            // 0xFF = UTWOR NIEOKRESLONY. Radio wysyla je, gdy z listy wybrano
            // sama PLYTE — wtedy zmieniarka gra ja od poczatku. Ta sama
            // konwencja co czas "nieznany" (0xFF) w ramce 0xC0. Wczesniej
            // wpadalo to w galaz F-padded BCD i dawalo utwor 15, ktory nie
            // istnieje — stad "CD10 TR15 (poza zakresem, ignoruje)".
            if (raw == 0xFF || raw == 0x00) {
                track = 1;
            } else if ((raw & 0xF0) == 0xF0) {
                track = (uint8_t)(raw & 0x0F);
            } else {
                track = UnilinkFrame::decodeBcd(raw);
            }
            if (track == 0) track = 1;
        }

        // Walidacja płyty: 1..10, obecna w magazynku
        bool discValid = (disc >= 1 && disc <= 10) && ((Magazine::presenceMap() >> (disc - 1)) & 1);
        // Walidacja utworu: 1..MAX_TRACKS, w granicach dostępnych na danej plycie
        uint8_t maxTrack = audioGetTrackCount(disc);
        if (maxTrack == 0) maxTrack = MAX_TRACKS;  // fallback bez nosnika
        bool trackValid = (track >= 1 && track <= maxTrack);

        if (discValid && trackValid) {
            // Zmiana plyty/utworu — wejdz w SEEK i zakolejkuj status
            CdChanger::selectDiscTrack(disc, track);
            onDiscChanged(disc);
            Serial.printf(">> 0xB0: CD%d TR%d (zakres OK)\n", disc, track);
        } else {
            // Zdarzenie rzadkie, wiec logujemy CALA ramke — bez niej nie da sie
            // odtworzyc, jak radio zakodowalo zadanie.
            Serial.printf(">> 0xB0 ODRZUCONE: CD%d TR%d (plyta %s, utwor %s, max=%d) raw:",
                          disc, track,
                          discValid ? "ok" : "BRAK",
                          trackValid ? "ok" : "POZA ZAKRESEM", maxTrack);
            for (int i = 0; i < len; i++) Serial.printf(" %02X", buf[i]);
            Serial.println();
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
        enqueueMagazineMiddle(0x97, 0x01, data);
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
// Dokladny uklad pol i wartosci — patrz komentarz przy buildStatusC0 nizej.

// Zbuduj ramke 0xC0 (long, status odtwarzania) 1:1 wg sniffu prawdziwej
// zmieniarki na CDX-M670:
//
//   70 31 C0 40 | A1 | 00 00 00 00 00 F1 F0 00 11 | 93 | 00   (CD1 TR1 00:00)
//   70 31 C0 20 | 81 | 00 00 00 00 00 F1 FF FF 80 | F0 | 00   (CD8, ladowanie)
//
// Kodowanie pol:
//   CMD2        = bajt statusu mechanizmu (0x00 gra, 0x20 zmiana plyty,
//                 0x40 ladowanie, 0x80 idle, 0xC0 wysuwanie) — ten sam, ktory
//                 idzie w PONG na `01 12`.
//   D1..D5      = 0x00
//   D6 (byte10) = TRK   — numer utworu, BCD z F-paddingiem (F1..F9, 10..99)
//   D7 (byte11) = MIN   — minuty, BCD z F-paddingiem; 0xFF gdy czas nieznany
//   D8 (byte12) = SEK   — sekundy, zwykle BCD; 0xFF gdy czas nieznany
//   D9 (byte13) = DISC  — numer plyty w STARSZYM nibblu (0x10=CD1 ... 0xA0=CD10),
//                 w dolnym nibblu flaga "ekran sie zmienil" (0 albo 1)
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
    CdChanger::MechState ms = CdChanger::mechState();

    // CMD2 = BAJT STATUSU MECHANIZMU — dokladnie ten sam, ktory odsylamy w PONG
    // na `01 12`. W sniffie CDX-M670 korelacja jest jednoznaczna:
    //   ping -> `10 31 00 40`, a rownolegle `70 31 C0 40 ...` (ladowanie)
    //   ping -> `10 31 00 20`, a rownolegle `70 31 C0 20 ...` (zmiana plyty)
    // Wczesniej wysylalismy tu na sztywno 0x00 (Playing) niezaleznie od stanu,
    // przez co radio podczas ladowania/zmiany plyty dostawalo sprzeczne
    // informacje (status 0x40 w PONG, 0x00 w ramce ekranu).
    const uint8_t st = statusByteFromState(ms);

    // Czas jest znany tylko wtedy, gdy mechanizm faktycznie odtwarza. Podczas
    // ladowania/zmiany plyty prawdziwa zmieniarka wysyla 0xFF w minutach i
    // sekundach (ekran pokazuje "--:--") — patrz `... F1 FF FF 80 ...` w sniffie.
    const bool timeKnown = (ms == CdChanger::MechState::Playing ||
                            ms == CdChanger::MechState::Seeking);
    const uint8_t trkB = UnilinkFrame::encodeBcdFpad(track);
    const uint8_t minB = timeKnown ? UnilinkFrame::encodeBcdFpad(min) : 0xFF;
    const uint8_t secB = timeKnown ? UnilinkFrame::encodeBcd(sec)     : 0xFF;

    // D9 = numer plyty w GORNYM nibblu, w dolnym flaga "tresc ekranu sie
    // zmienila". Sniff CDX-M670: 0x11/0x10 na CD1, 0x21..0x81 podczas skakania
    // po plytach, 0x80/0x81 na CD8 — dolny nibbel to zawsze 0 albo 1.
    // Wczesniej wpisywalismy tu na stale 0x08 (czyli np. 0x18 dla CD1), co jest
    // wzorcem z ramki 0x90, a NIE z 0xC0.
    const uint8_t discB = UnilinkFrame::discHighNibble(disc, CdChanger::isDisplayDirty() ? 0x01 : 0x00);

    frame[0]  = 0x70;                      // RAD = display
    frame[1]  = myAddr;                    // TAD = nasz adres
    frame[2]  = 0xC0;                      // CMD1 = 0xC0 (status odtwarzania)
    frame[3]  = st;                        // CMD2 = bajt statusu mechanizmu
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = 0x00;                      // D1 = rezerwa
    frame[6]  = 0x00;                      // D2 = rezerwa
    frame[7]  = 0x00;                      // D3 = rezerwa
    frame[8]  = 0x00;                      // D4 = rezerwa
    // D5 = 0x30 gdy w slocie FAKTYCZNIE jest plyta, 0x00 gdy slot pusty.
    // Zrzut prawdziwej zmieniarki (jedna plyta, CD8) — te same RAD/TAD/CMD1/CMD2,
    // rozni sie tylko D5 i numer plyty w D9:
    //   70 31 C0 40 A1 00 00 00 00 30 F1 F0 00 88 3A 00   (CD8 — plyta w slocie)
    //   70 31 C0 40 A1 00 00 00 00 00 F1 F0 00 11 93 00   (CD1 — slot pusty)
    // Emulator wysylal tu na sztywno 0x00, czyli melodowal radiu "slot pusty"
    // przy KAZDEJ plycie — stad brak nazw na liscie i brak zapytan o CD-TEXT.
    const bool discPresent = audioGetTrackCount(disc) > 0;
    frame[9]  = discPresent ? 0x30 : 0x00; // D5 = obecnosc plyty
    frame[10] = trkB;                      // D6 = numer utworu (F-padded BCD)
    frame[11] = minB;                      // D7 = minuty (F-padded BCD / 0xFF)
    frame[12] = secB;                      // D8 = sekundy (BCD / 0xFF)
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

// Wybierz i NADAJ swiezy ekran na grant 0x13 przy pustej kolejce.
// CDX-M670: czas na ekranie bierze z ramki 0xC0 (D7/D8). Podczas LOAD/ChangedCd
// prawdziwa zmieniarka wysyla 0xFF (= "--.--"); po wejsciu w Playing MUSI
// dojsc 0xC0 z prawdziwym czasem — sam 0x90 nie zdejmuje "--.--" (potwierdzone
// logiem 15:34). Cadencja jak w sniffe: 0xC0 ~1 Hz (gdy zmienia sie sekunda),
// miedzy nimi lekki 0x90 (interpolacja).
// Ekran BEZCZYNNEJ zmieniarki: ramka 0x8E. W sniffie CDX-M670, gdy zmieniarka
// stoi z wybrana plyta i nie gra, na kazdy grant leci dokladnie:
//   70 31 8E C0 | EF | 00 00 00 80 | 6F | 00       (CD8)
// czyli CMD2=0xC0, D1..D3=0, D4 = numer plyty w gornym nibblu. Wczesniej
// wysylalismy tu ramke czasu 0xC0 z zerowym licznikiem, przez co radio w stanie
// spoczynku pokazywalo "00:00" zamiast normalnego ekranu zmieniarki.
static void sendIdleScreen0x8E() {
    uint8_t frame[11];
    frame[0]  = 0x70;                  // RAD = wyswietlacz
    frame[1]  = myAddr;                // TAD = nasz adres
    frame[2]  = 0x8E;                  // CMD1 = 0x8E (middle, ekran spoczynku)
    frame[3]  = 0xC0;                  // CMD2
    frame[4]  = UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
    frame[5]  = 0x00;                  // D1
    frame[6]  = 0x00;                  // D2
    frame[7]  = 0x00;                  // D3
    frame[8]  = UnilinkFrame::discHighNibble(CdChanger::disk(), 0x00);   // D4
    frame[9]  = UnilinkFrame::parity2(frame[4], &frame[5], 4);
    frame[10] = 0x00;                  // END
    UnilinkBus::sendRaw(frame, sizeof(frame));
}

static void sendFreshDisplay() {
    uint8_t disc  = CdChanger::disk();
    uint8_t track = CdChanger::track();
    uint8_t min   = CdChanger::minutes();
    uint8_t sec   = CdChanger::seconds();
    CdChanger::MechState ms = CdChanger::mechState();

    // Czy od ostatniej oddanej ramki zmienila sie plyta albo utwor? Musimy to
    // policzyc PRZED aktualizacja s_lastShown*, bo od tego zalezy wybor miedzy
    // pelnym 0xC0 a lekkim tikiem 0x90.
    const bool changed = (disc != s_lastShownDisc || track != s_lastShownTrack);

    // Cokolwiek zaraz nadamy, radio bedzie mialo na ekranie TEN stan. Zapis musi
    // objac KAZDA sciezke (takze 0x8E i lekki 0x90), bo na tych trzech polach
    // opiera sie wykrywanie "ekran nieaktualny" (displayStale) — inaczej pilny
    // Break powtarzalby sie w kolko po kazdej zmianie plyty.
    s_lastShownDisc  = disc;
    s_lastShownTrack = track;
    s_lastShownState = statusByteFromState(ms);

    // Mechanizm stoi (Init/Idle/Ejecting) — nie ma czasu do pokazania.
    if (ms == CdChanger::MechState::Idle || ms == CdChanger::MechState::Init) {
        sendIdleScreen0x8E();
        s_lastC0Min = s_lastC0Sec = 0xFF;
        if (DEBUG_FRAMES) Serial.printf("   TX 0x8E (idle) CD%d\n", disc);
        return;
    }

    // [NAPRAWA SYSTEM RESET] Pelny 0xC0 (16 bajtow) TYLKO przy zmianie plyty/
    // utworu lub w stanach przejsciowych. W normalnym Playing wysylamy lekki
    // 0x90 tick (11 bajtow) — to wystarczy do interpolacji czasu na radiu.
    // Pelny 0xC0 co sekunde (stary kod) generowal ciezkie 16-bajtowe TX,
    // ktore powodowaly wiecej RESYNCow i SYSTEM RESETow.
    // Okresowy 0xC0 idzie z serviceFullStatusFrame (co 5s z kolejki).
    if (changed || ms == CdChanger::MechState::Seeking ||
        ms == CdChanger::MechState::LoadingTrack ||
        ms == CdChanger::MechState::ChangedCd) {
        if (ms == CdChanger::MechState::Playing || ms == CdChanger::MechState::Seeking) {
            s_lastC0Min = min;
            s_lastC0Sec = sec;
        } else {
            s_lastC0Min = s_lastC0Sec = 0xFF;
        }
        sendFreshStatusC0();
        if (DEBUG_FRAMES) Serial.printf("   TX 0xC0 CD%d TR%d %02d:%02d\n",
                                        disc, track, min, sec);
        return;
    }

    // Playing, ta sama plyta/utwor — lekki tik 0x90 (interpolacja czasu).
    uint8_t frame[11];
    buildLightTick0x90(frame);
    UnilinkBus::sendRaw(frame, sizeof(frame));
    if (DEBUG_FRAMES) Serial.printf("   TX 0x90 CD%d TR%d %02d:%02d\n",
                                    disc, track, min, sec);
}

// Natychmiastowy push pelnego statusu (najwyzszy priorytet) — przy zmianie
// utworu/plyty/stanu (API publiczne, deklaracja w .h).
void enqueueFullStatusFrame() {
    enqueueStatusC0(Tx::PRIO_STATUS);
}

// [NAPRAWA ZASTYGANIA] Harmonogram ramki 0xC0 — co ~5s w stanie Playing
// kolejkujemy pelny status. To uzupelnia lekki tik 0x90 i gwarantuje, ze
// radio okresowo dostaje kompletna informacje (numer plyty, utworu, czas).
// 5s to kompromis: nie zasmiecamy kolejki (jak przy 1s), a radio nie czeka
// zbyt dlugo na pelna aktualizacje.
static unsigned long lastFullStatusMs = 0;
void serviceFullStatusFrame(unsigned long now) {
    if (CdChanger::mechState() != CdChanger::MechState::Playing) {
        lastFullStatusMs = now;
        return;
    }
    if ((now - lastFullStatusMs) < 5000) return;
    lastFullStatusMs = now;
    enqueueStatusC0(Tx::PRIO_TIME);  // niski priorytet — nie wypycha wazniejszych ramek
}

// Natychmiastowe wysłanie pełnego statusu (poprawna ramka 0xC0 z markerem 0x30).
// Używana przy broadcastzie 0x08 (zakończenie przewijania) — R10.2.
void sendDisplayStatus() {
    enqueueFullStatusFrame();
}

} // namespace UnilinkProtocol
