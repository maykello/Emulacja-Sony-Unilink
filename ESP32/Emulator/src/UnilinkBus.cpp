// =============================================================================
// UnilinkBus.cpp — implementacja warstwy fizycznej magistrali Sony UniLink
// =============================================================================
#include "UnilinkBus.h"
#include "Config.h"
#include "Diagnostics.h"
#include "UnilinkFrame.h"  // jedno zrodlo prawdy dla sum kontrolnych (Parity1/Parity2)

namespace UnilinkBus {

// --- STROJENIE CZASOW (kompatybilne z protokolem §1) ---
// Okres bitu: ~20 µs (zgodnie z Kompendium §1). Przerwanie onClockEvent() sygnalizuje
// zbocze zegara i probkuje bit na linii DATA. Zmiana timingu bitu zaklocy komunikacje.
// Czas bajtu (8 bitów + przerwa): ~1 ms. Parity1/Parity2 sa liczone po odebraniu
// calych bajtow ramki (UnilinkFrame::parity1/parity2).
// Slave Break musi trafic w faze HIGH fali idle na linii DATA (patrz sekcja
// SLAVE BREAK na koncu tego pliku).
// [HIGH-RISK] Zmiana timingu Slave Break (BREAK_* w Config.h) moze spowodowac
// kolizje albo brak wykrycia breaka przez radio.

// --- STAN ODBIORU (RX) ---
static volatile uint8_t       rxBuffer[RX_BUFFER_SIZE];
static volatile uint8_t       rxIncomingByte = 0;
static volatile int           rxBitIndex     = 0;
static volatile int           rxIndex        = 0;
static volatile unsigned long lastClockTime  = 0;

// --- STAN NADAWANIA (TX) ---
static volatile bool    isAnswering = false;
static volatile uint8_t txBuffer[TX_BUFFER_SIZE];
static volatile int     txIndex     = 0;
static volatile int     txBitIndex  = 0;
static volatile int     txLength    = 0;

// Ustawienie bitu na linii DATA (z uwzglednieniem sprzetowego inwertera).
static inline void setTxData(bool bitVal) {
    bool outVal = INVERT_DATA ? !bitVal : bitVal;
    digitalWrite(PIN_DATA, outVal ? HIGH : LOW);
}

// Odczyt LOGICZNEGO poziomu DATA (z uwzglednieniem sprzetowego inwertera).
// Uzywane przez maszyne Slave Break do synchronizacji z fala idle (§2.2).
static inline bool readDataLogic() {
    bool v = digitalRead(PIN_DATA);
    return INVERT_DATA ? !v : v;
}

// Przerwanie zegara: nadaje lub odbiera pojedynczy bit.
static void IRAM_ATTR onClockEvent() {
    const unsigned long nowUs = micros();
    const unsigned long gapUs = nowUs - lastClockTime;
    lastClockTime = nowUs;

    if (isAnswering) {
        if (txIndex < txLength) {
            uint8_t currentByte = txBuffer[txIndex];
            bool bitVal = (currentByte >> (7 - txBitIndex)) & 0x01;
            setTxData(bitVal);

            txBitIndex++;
            if (txBitIndex > 7) {
                txBitIndex = 0;
                txIndex++;
                if (txIndex >= txLength) {
                    isAnswering = false;
                    pinMode(PIN_DATA, INPUT);
                    // --- FLUSH ECHA TX ---
                    // Podczas nadawania master taktuje, a my ustawiamy DATA na
                    // magistrali wired-OR. ISR rownoczesnie ODBIERA te same bity
                    // do rxBuffer — to echo naszej wlasnej transmisji. Bez
                    // flushowania rxIndex te bajty tworza "widmowe ramki" (np.
                    // `10 18 82 01 AB ...` odczytane jako odpowiedz obcego
                    // urzadzenia), ktore zaburzaja dalsze parsowanie i powoduja
                    // utrate kolejnych ramek (w tym krytycznego Time Poll 01 12).
                    rxIndex = 0;
                    rxBitIndex = 0;
                    rxIncomingByte = 0;
                }
            }
        }
    } else {
        // --- RESYNCHRONIZACJA FAZY BITOWEJ NA PRZERWIE MIEDZY RAMKAMI ---
        // Wewnatrz ramki zbocza zegara ida co ~117 us bez zadnej przerwy miedzy
        // bajtami (wyliczone ze znacznikow `t=` w sniffie: ramka 16-bajtowa trwa
        // 14965-15000 us, czyli ~937 us/bajt). Kolejne ramki dzieli natomiast co
        // najmniej ~5970 us. Przerwa dluzsza niz BYTE_RESYNC_GAP_US oznacza wiec
        // POCZATEK NOWEJ RAMKI i musi zerowac licznik bitow.
        //
        // Bez tego pojedyncze zgubione zbocze (petla robi tez audio, USB host,
        // Serial 921600 i zapisy NVS) przesuwalo faze na STALE: caly strumien szedl
        // dalej przesuniety o bit i zamiast `18 10 01 15 3E 00` odbieralismy
        // `30 20 02 2A 7C`. W logu widac dokladnie takie ramki-widma. Faza wracala
        // dopiero przy awaryjnym czyszczeniu bufora w readFrame.
        if (gapUs > BYTE_RESYNC_GAP_US) {
            rxBitIndex = 0;
            rxIncomingByte = 0;
        }

        bool bitVal = digitalRead(PIN_DATA);
        if (INVERT_DATA) bitVal = !bitVal;

        if (bitVal) {
            rxIncomingByte |= (1 << (7 - rxBitIndex));
        } else {
            rxIncomingByte &= ~(1 << (7 - rxBitIndex));
        }

        rxBitIndex++;
        if (rxBitIndex > 7) {
            // --- SYNCHRONIZACJA POCZATKU RAMKI ---
            // Master taktuje po kazdej ramce jeszcze JEDEN pusty slot bajtu
            // (widoczny w sniffie jako samotne `00` ~6 ms po ramce). Gdyby taki
            // bajt wypelniacza trafil do bufora jako RAD, cala reszta strumienia
            // przesunelaby sie o jeden bajt i KAZDA kolejna ramka bylaby
            // odrzucana na parzystosci — az do najblizszej dluzszej ciszy.
            // Pierwszym bajtem ramki jest RAD, ktory ZAWSZE ma niezerowy gorny
            // nibbel (0x10 master, 0x18 broadcast, 0x3X zmieniarki, 0x70 ekran,
            // 0x9X...). Bajt < 0x10 na pozycji 0 to wiec wypelniacz — gubimy go.
            const bool plausibleAddress = (rxIncomingByte >= 0x10);
            if (rxIndex != 0 || plausibleAddress) {
                if (rxIndex < RX_BUFFER_SIZE) {
                    rxBuffer[rxIndex++] = rxIncomingByte;
                }
            }
            rxBitIndex = 0;
            rxIncomingByte = 0;
        }
    }
}

// Rozpoczyna nadawanie gotowego pakietu (wspolny kod dla wszystkich sendXxx).
static void startTransmit(const uint8_t* pkt, int len) {
    if (len > TX_BUFFER_SIZE) return;
    Diagnostics::recordFrame("TX", pkt, len);
    noInterrupts();
    for (int i = 0; i < len; i++) txBuffer[i] = pkt[i];
    txLength    = len;
    txIndex     = 0;
    txBitIndex  = 0;
    isAnswering = true;
    pinMode(PIN_DATA, OUTPUT);
    interrupts();
}

void begin() {
    pinMode(PIN_BUS_ON, INPUT);
    pinMode(PIN_CLOCK, INPUT);
    pinMode(PIN_DATA, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_CLOCK), onClockEvent, CLOCK_EDGE);
    lastClockTime = micros();
}

void sendShort(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2) {
    uint8_t pkt[6] = {
        rad, tad, cmd1, cmd2,
        UnilinkFrame::parity1(rad, tad, cmd1, cmd2),  // Parity1
        0x00                                          // End byte
    };
    startTransmit(pkt, sizeof(pkt));
}

void sendMedium(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2,
                uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4) {
    uint8_t p1   = UnilinkFrame::parity1(rad, tad, cmd1, cmd2);
    uint8_t data[4] = { d1, d2, d3, d4 };
    uint8_t p2   = UnilinkFrame::parity2(p1, data, 4);
    uint8_t pkt[11] = {
        rad, tad, cmd1, cmd2,
        p1,                       // Parity1
        d1, d2, d3, d4,
        p2,                       // Parity2
        0x00
    };
    startTransmit(pkt, sizeof(pkt));
}

void sendLong(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2,
              uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4,
              uint8_t d5, uint8_t d6, uint8_t d7, uint8_t d8, uint8_t d9) {
    uint8_t p1   = UnilinkFrame::parity1(rad, tad, cmd1, cmd2);
    uint8_t data[9] = { d1, d2, d3, d4, d5, d6, d7, d8, d9 };
    uint8_t p2   = UnilinkFrame::parity2(p1, data, 9);
    uint8_t pkt[16] = {
        rad, tad, cmd1, cmd2, p1,
        d1, d2, d3, d4, d5, d6, d7, d8, d9,
        p2, 0x00
    };
    startTransmit(pkt, sizeof(pkt));
}

void sendRaw(const uint8_t* data, int len) {
    startTransmit(data, len);
}

bool isTransmitting() {
    return isAnswering;
}

unsigned long microsSinceLastClock() {
    noInterrupts();
    unsigned long last = lastClockTime;
    interrupts();
    return micros() - last;
}

int readPacketIfIdle(uint8_t* out, int maxLen, unsigned long idleUs) {
    noInterrupts();
    bool idle = (micros() - lastClockTime > idleUs);
    int count = rxIndex;
    if (!idle || count <= 0) {
        interrupts();
        return 0;
    }
    if (count > maxLen) count = maxLen;
    for (int i = 0; i < count; i++) out[i] = rxBuffer[i];
    rxIndex = 0;
    rxBitIndex = 0;
    interrupts();
    return count;
}

int readFrame(uint8_t* out, int maxLen) {
    if (maxLen <= 0) return 0;
    noInterrupts();
    int count = rxIndex;

    // --- RESYNCHRONIZACJA PO PARITY1 ---
    // KAZDY CMD1 ma przypisana dlugosc, wiec sama dlugosc nie wykryje przesuniecia
    // bajtowego. Robi to Parity1: jest liczona z pierwszych czterech bajtow i
    // musi sie zgadzac z piatym. Gdy sie nie zgadza, prawie na pewno zaczelismy
    // skladac ramke od zlego bajtu — zrzucamy jeden bajt i probujemy od nastepnego,
    // zamiast konsumowac (i psuc) caly nastepny pakiet.
    if (count >= 5) {
        const uint8_t p1 = UnilinkFrame::parity1(rxBuffer[0], rxBuffer[1],
                                                 rxBuffer[2], rxBuffer[3]);
        if (rxBuffer[4] != p1) {
            for (int i = 0; i + 1 < count; i++) rxBuffer[i] = rxBuffer[i + 1];
            rxIndex = count - 1;
            interrupts();
            Diagnostics::recordNote("RESYNC");
            return 0;
        }
    }

    // --- KRYTERIUM PODSTAWOWE: granica ramki wyznaczona przez CMD1 (R3.4/R3.5) ---
    // Uklad bufora: rxBuffer[0]=RAD, [1]=TAD, [2]=CMD1. Gdy mamy >= 3 bajty,
    // znamy CMD1 i dlugosc calej ramki. Udostepniamy ja dopiero gdy zebrano
    // >= expected bajtow; ewentualna nadwyzka to poczatek kolejnej ramki i
    // zostaje w buforze (ciecie strumienia po granicach z CMD1, nie po ciszy).
    if (count >= 3) {
        uint8_t cmd1 = rxBuffer[2];
        int expected = UnilinkFrame::lengthFromCmd1(cmd1);
        if (count >= expected) {
            int n = expected;
            if (n > maxLen) n = maxLen;
            for (int i = 0; i < n; i++) out[i] = rxBuffer[i];
            // przesun nadmiarowe bajty (poczatek nastepnej ramki) na poczatek bufora
            int remaining = count - expected;
            for (int i = 0; i < remaining; i++) rxBuffer[i] = rxBuffer[expected + i];
            rxIndex = remaining;
            interrupts();
            return n;
        }
    }

    // --- ZABEZPIECZENIE AWARYJNE: resynchronizacja po ciszy (R3.5) ---
    // Gdy po READ_SILENCE_US w buforze tkwi niekompletny zlepek (count < 3 albo
    // count < expected), ODRZUCAMY go bez dostarczania do handlePacket.
    // Wczesniej zwracalismy smieci (np. `10 01 15 3C` zamiast `18 10 01 15 3E 00`)
    // — len<6 i tak je odrzucal, ale DEBUG_FRAMES logowal je jako RX, a czesc
    // bajtow mogla byc poczatkiem prawdziwej ramki ucietej przez kolizje.
    bool idle = (micros() - lastClockTime > READ_SILENCE_US);
    if (idle && count > 0) {
        rxIndex = 0;
        rxBitIndex = 0;
        rxIncomingByte = 0;
        interrupts();
        Diagnostics::recordNote("RXFLUSH");
        return 0;
    }

    interrupts();
    return 0;
}

void resetRx() {
    noInterrupts();
    rxIndex = 0;
    rxBitIndex = 0;
    rxIncomingByte = 0;
    interrupts();
}

// =============================================================================
// SLAVE BREAK — zsynchronizowany z fala idle linii DATA
// =============================================================================
// Gdy magistrala jest bezczynna, master generuje na linii DATA fale prostokatna
// 8 ms LOW / 8 ms HIGH. Slave zglasza chec nadawania WYLACZNIE wewnatrz fazy
// HIGH: czeka az zobaczy pelna faze LOW, potem 2 ms po zboczu w gore sciaga DATA
// do zera na ~2 ms i puszcza. Dzieki temu break miesci sie w oknie, w ktorym
// zaden inny slave ani master nie nadaje.
//
// Poprzednia implementacja IGNOROWALA fale DATA i sciagala linie po samej ciszy
// zegara. Trafiala wiec w losowa faze — czesto w moment, gdy master zaczynal
// takt — co niszczylo ramke i konczylo sie petla SYSTEM RESET radia. Dodatkowo
// blokowala petle glowna na czas trzymania linii.
//
// Teraz jest to NIEBLOKUJACA maszyna stanow: `requestSlaveBreak()` ja uzbraja,
// a `serviceSlaveBreak()` (wolane w kazdej iteracji loop()) przesuwa ja o krok.
// Kazda aktywnosc zegara natychmiast ja przerywa — nigdy nie kolidujemy.
namespace {

enum class BreakState : uint8_t {
    Idle,        // nieuzbrojony
    WaitLow,     // czekam na poczatek fazy LOW fali idle
    ConfirmLow,  // faza LOW musi potrwac BREAK_IDLE_LOW_US
    WaitHigh,    // czekam na zbocze w gore
    Settle,      // 2 ms w fazie HIGH przed sciagnieciem linii
    Hold,        // trzymam DATA LOW
};

BreakState    s_breakState   = BreakState::Idle;
unsigned long s_breakMark    = 0;   // micros() poczatku biezacej fazy
unsigned long s_breakArmedAt = 0;   // micros() uzbrojenia (bezpiecznik)
unsigned long s_breakClockRef = 0;  // lastClockTime w chwili wejscia w faze
volatile uint16_t s_breakCompleted = 0;  // ile Hold ukonczono (wykrywalny break)
unsigned long s_breakDoneMs = 0;    // millis() ostatniego udanego Hold

// Czy od `ref` nie bylo zadnego zbocza zegara (magistrala nadal bezczynna)?
inline bool busStillQuiet(unsigned long ref) {
    noInterrupts();
    unsigned long lc = lastClockTime;
    interrupts();
    return lc == ref;
}

// Wejscie w Hold: wystaw na DATA poziom DOMINUJACY — Mictronics 3 ms w fazie
// HIGH fali idle.
//
// Poziomem dominujacym jest LOGICZNA JEDYNKA. Dwa niezalezne dowody z tego
// samego strumienia, ktory poprawnie dekodujemy:
//   * maski arbitrazu sumuja sie bitowo (`82 01` od 0x3B i `82 04` od nas daja
//     `82 05`) — tak zachowuje sie tylko poziom dominujacy = 1,
//   * pusty slot bajtu, ktory master taktuje po kazdej ramce, czyta sie jako
//     0x00 — czyli stan RECESYWNY (nikt nie nadaje) to logiczne 0.
//
// Wczesniej bylo tu setTxData(false), czyli poziom RECESYWNY — dokladnie ten,
// ktory linia ma juz w fazie HIGH fali idle. Break nie zmienial wiec NICZEGO na
// magistrali: maszyna stanow raportowala Hold "OK", master nigdy go nie widzial
// i Request Polling (`01 15`) nie wracal.
inline void enterHold(unsigned long now) {
    pinMode(PIN_DATA, OUTPUT);
    setTxData(true);
    Diagnostics::recordNote("BREAK");
    noInterrupts();
    s_breakClockRef = lastClockTime;
    interrupts();
    s_breakState = BreakState::Hold;
    s_breakMark  = now;
}

inline void finishHoldSuccess() {
    pinMode(PIN_DATA, INPUT);
    s_breakState = BreakState::Idle;
    s_breakCompleted++;
    s_breakDoneMs = millis();
    noInterrupts();
    rxBitIndex = 0;
    rxIncomingByte = 0;
    interrupts();
}

inline void finishHoldAbort() {
    pinMode(PIN_DATA, INPUT);
    s_breakState = BreakState::Idle;
    noInterrupts();
    rxBitIndex = 0;
    rxIncomingByte = 0;
    interrupts();
}

} // namespace

void requestSlaveBreak() {
    if (s_breakState != BreakState::Idle) return;
    s_breakState   = BreakState::WaitLow;
    s_breakMark    = micros();
    s_breakArmedAt = s_breakMark;
    noInterrupts();
    s_breakClockRef = lastClockTime;
    interrupts();
}

bool slaveBreakPending() {
    return s_breakState != BreakState::Idle;
}

void cancelSlaveBreak() {
    if (s_breakState == BreakState::Hold) {
        pinMode(PIN_DATA, INPUT);
    }
    s_breakState = BreakState::Idle;
}

uint16_t takeBreakCompleted() {
    noInterrupts();
    uint16_t n = s_breakCompleted;
    s_breakCompleted = 0;
    interrupts();
    return n;
}

bool breakRecoveryActive(unsigned long nowMs) {
    if (s_breakDoneMs == 0) return false;
    return (nowMs - s_breakDoneMs) < BREAK_RECOVERY_MS;
}

void serviceSlaveBreak() {
    if (s_breakState == BreakState::Idle) return;

    const unsigned long now = micros();

    if (now - s_breakArmedAt > BREAK_ARM_TIMEOUT_US) {
        cancelSlaveBreak();
        return;
    }

    // Hold — Mictronics: pelne 3 ms LOW w fazie HIGH, potem Hi-Z na reszte HIGH.
    // SophWiki: w fazie HIGH bywa 8-bitowy filler clock — NIE przerywamy impulsu
    // na wczesnym zboczu (inaczej master nie wykrywa Break; log: Hold „OK”
    // bez powrotu `01 15`). Po HOLD_US puszczamy nawet gdy master juz taktuje.
    if (s_breakState == BreakState::Hold) {
        if (now - s_breakMark >= BREAK_HOLD_US) {
            finishHoldSuccess();
        }
        return;
    }

    // Zegar = magistrala aktywna — tylko pelna fala idle (NIE "fallback" na
    // ciszy 5 ms: przerwy miedzy ramkami Request Polling sa ~6 ms i fallback
    // wstrzeliwal Break w srodek wymiany → RESYNC → 18 10 01 00).
    if (!busStillQuiet(s_breakClockRef)) {
        noInterrupts();
        s_breakClockRef = lastClockTime;
        interrupts();
        s_breakState = BreakState::WaitLow;
        s_breakMark  = now;
        return;
    }

    switch (s_breakState) {
        // Ponizej "faza LOW/HIGH" opisuje fale idle tak, jak widzi ja MASTER na
        // linii DATA. W naszej reprezentacji logicznej (readDataLogic):
        //   faza LOW  = poziom dominujacy = logiczna 1 = readDataLogic() true,
        //   faza HIGH = poziom recesywny  = logiczne 0 = readDataLogic() false.
        // Break wstrzeliwujemy w faze HIGH — tam i tylko tam wystawienie poziomu
        // dominujacego (enterHold) jest dla mastera widoczna zmiana stanu.
        case BreakState::WaitLow:
            if (readDataLogic()) {
                s_breakState = BreakState::ConfirmLow;
                s_breakMark  = now;
            }
            break;

        case BreakState::ConfirmLow:
            if (!readDataLogic()) {
                // Linia wrocila do HIGH. Sprawdzamy czy faza LOW trwala wystarczajaco dlugo.
                if (now - s_breakMark >= BREAK_IDLE_LOW_MIN_US) {
                    // LOW trwalo >= 6ms i wlasnie przeszlo w HIGH. Od razu przechodzimy do Settle!
                    s_breakState = BreakState::Settle;
                    s_breakMark  = now;
                } else {
                    // Za krotki LOW (zaklocenie), szukamy od nowa.
                    s_breakState = BreakState::WaitLow;
                    s_breakMark  = now;
                }
            } else if (now - s_breakMark >= BREAK_IDLE_LOW_US) {
                // Linia jest LOW juz pelne 8ms. Czekamy az przejdzie w HIGH.
                s_breakState = BreakState::WaitHigh;
                s_breakMark  = now;
            }
            break;

        case BreakState::WaitHigh:
            if (!readDataLogic()) {
                // Przejscie LOW -> HIGH. Odliczamy 2ms.
                s_breakState = BreakState::Settle;
                s_breakMark  = now;
            }
            break;

        case BreakState::Settle:
            if (readDataLogic()) {
                // Linia wrocila do LOW zanim minelo 2ms! To nie jest fala idle.
                s_breakState = BreakState::WaitLow;
                s_breakMark  = now;
            } else if (now - s_breakMark >= BREAK_SETTLE_US) {
                // Linia jest HIGH przez 2ms. Czas na HOLD (3ms poziomu dominujacego).
                enterHold(now);
            }
            break;

        case BreakState::Hold:
        case BreakState::Idle:
        default:
            break;
    }
}

} // namespace UnilinkBus
