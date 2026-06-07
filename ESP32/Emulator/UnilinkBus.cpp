// =============================================================================
// UnilinkBus.cpp — implementacja warstwy fizycznej magistrali Sony UniLink
// =============================================================================
#include "UnilinkBus.h"
#include "Config.h"
#include "Diagnostics.h"

namespace UnilinkBus {

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
        (uint8_t)(rad + tad + cmd1 + cmd2),  // Parity
        0x00                                  // End byte
    };
    startTransmit(pkt, sizeof(pkt));
}

void sendMedium(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2,
                uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4) {
    uint8_t pkt[11] = {
        rad, tad, cmd1, cmd2,
        (uint8_t)(rad + tad + cmd1 + cmd2),                    // Parity1
        d1, d2, d3, d4,
        (uint8_t)(rad + tad + cmd1 + cmd2 + d1 + d2 + d3 + d4),// Parity2
        0x00
    };
    startTransmit(pkt, sizeof(pkt));
}

void sendLong(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2,
              uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4,
              uint8_t d5, uint8_t d6, uint8_t d7, uint8_t d8, uint8_t d9) {
    uint8_t sum1 = (uint8_t)(rad + tad + cmd1 + cmd2);
    uint8_t sum2 = (uint8_t)(sum1 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9);
    uint8_t pkt[16] = {
        rad, tad, cmd1, cmd2, sum1,
        d1, d2, d3, d4, d5, d6, d7, d8, d9,
        sum2, 0x00
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
