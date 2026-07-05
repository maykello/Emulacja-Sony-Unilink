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
// Slave Break: trzeba czekac na cisze ~8 ms (BREAK_SILENCE_US) przed jego wystawieniem.
// [HIGH-RISK] Zmiana timinguSlave Break (BREAK_SILENCE_US/BREAK_HOLD_US w Config.h)
// moze spowodowac kolizje lub brak wykrycia break przez radio.

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
// Uzywane przez issueSlaveBreak do synchronizacji z fala idle (§2.2).
static inline bool readDataLogic() {
    bool v = digitalRead(PIN_DATA);
    return INVERT_DATA ? !v : v;
}

// Przerwanie zegara: nadaje lub odbiera pojedynczy bit.
static void IRAM_ATTR onClockEvent() {
    lastClockTime = micros();

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
                }
            }
        }
    } else {
        bool bitVal = digitalRead(PIN_DATA);
        if (INVERT_DATA) bitVal = !bitVal;

        if (bitVal) {
            rxIncomingByte |= (1 << (7 - rxBitIndex));
        } else {
            rxIncomingByte &= ~(1 << (7 - rxBitIndex));
        }

        rxBitIndex++;
        if (rxBitIndex > 7) {
            if (rxIndex < RX_BUFFER_SIZE) {
                rxBuffer[rxIndex++] = rxIncomingByte;
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

    // --- KRYTERIUM PODSTAWOWE: granica ramki wyznaczona przez CMD1 (R3.4/R3.5) ---
    // Uklad bufora: rxBuffer[0]=RAD, [1]=TAD, [2]=CMD1. Gdy mamy >= 3 bajty,
    // znamy CMD1 i dlugosc calej ramki. Udostepniamy ja dopiero gdy zebrano
    // >= expected bajtow; ewentualna nadwyzka to poczatek kolejnej ramki i
    // zostaje w buforze (ciecie strumienia po granicach z CMD1, nie po ciszy).
    if (count >= 3) {
        uint8_t cmd1 = rxBuffer[2];
        int expected = UnilinkFrame::lengthFromCmd1(cmd1);
        if (expected > 0 && count >= expected) {
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
    // count < expected) lub nieznana ramka, oprozniamy bufor (jak dotychczas),
    // by uniknac zakleszczenia. Wartosci czasowe pozostaja nietkniete.
    bool idle = (micros() - lastClockTime > READ_SILENCE_US);
    if (idle && count > 0) {
        int n = count;
        if (n > maxLen) n = maxLen;
        for (int i = 0; i < n; i++) out[i] = rxBuffer[i];
        rxIndex = 0;
        rxBitIndex = 0;
        interrupts();
        return n;
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

void issueSlaveBreak() {
    // Ostateczna kontrola ciszy TUZ przed pociagnieciem (clock mogl wlasnie przyjsc).
    noInterrupts();
    unsigned long lastClk = lastClockTime;
    bool stillIdle = (micros() - lastClk > BREAK_SILENCE_US);
    interrupts();
    if (!stillIdle) return;  // radio zaczelo nadawac — NIE przerywaj, odpusc break

    Diagnostics::recordNote("BREAK");
    pinMode(PIN_DATA, OUTPUT);
    digitalWrite(PIN_DATA, LOW);
    // Trzymaj DATA nisko, ale PRZERWIJ natychmiast gdy radio ruszy z zegarem.
    // Trzymanie na sztywno niszczylo pakiet radia -> SYSTEM RESET.
    unsigned long t0 = micros();
    while ((micros() - t0) < BREAK_HOLD_US) {
        noInterrupts();
        unsigned long lc = lastClockTime;
        interrupts();
        if (lc != lastClk) break;  // przyszedl clock = radio nadaje -> puszczamy
    }
    pinMode(PIN_DATA, INPUT);
    // Reset stanu odbiornika — uniknij interpretacji "ogona" break-a jako bajtu.
    noInterrupts();
    rxBitIndex = 0;
    rxIncomingByte = 0;
    lastClockTime = micros();
    interrupts();
}

} // namespace UnilinkBus
