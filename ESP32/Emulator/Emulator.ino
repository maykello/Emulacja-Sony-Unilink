#include <Arduino.h>

// --- KONFIGURACJA PINÓW ---
#define PIN_BUS_ON  4
#define PIN_CLOCK   5
#define PIN_DATA    6

// --- USTAWIENIA SPRZĘTOWE ---
const bool INVERT_DATA = true;
const int CLOCK_EDGE = RISING; 

// --- ODBIERANIE (RX) ---
#define RX_BUFFER_SIZE 64
volatile uint8_t rxBuffer[RX_BUFFER_SIZE];
volatile uint8_t rxIncomingByte = 0;
volatile int rxBitIndex = 0;
volatile int rxIndex = 0;
volatile unsigned long lastClockTime = 0;

// --- NADAWANIE (TX) ---
volatile bool isAnswering = false;
volatile uint8_t txBuffer[64];
volatile int txIndex = 0;
volatile int txBitIndex = 0;
volatile int txLength = 0;

// === Zmienne Wirtualnej Zmieniarki ===
unsigned long lastSecondTick = 0;
uint8_t playSeconds = 0;
uint8_t playMinutes = 0;
uint8_t currentDisk = 1;
uint8_t currentTrack = 1; 

bool deviceAllocated = false;
unsigned long lastPingTime = 0;

// Stany maszyny:
// 0xC0 = Initializing (mechanizm się budzi)
// 0x80 = Stopped/Idle (gotowy, stoi)
// 0x40 = Loading disc (ładowanie płyty)
// 0x20 = Seeking track (szukanie)
// 0x00 = Playing (odtwarzanie)
uint8_t cdState = 0xC0;
unsigned long seekStartTime = 0;
unsigned long initWaitTime = 0;

// Slave Break - flagi
bool wantSlaveBreak = false;
unsigned long lastBreakTime = 0;
bool needDisplayUpdate = false;

void setTxData(bool bitVal) {
    bool outVal = INVERT_DATA ? !bitVal : bitVal;
    digitalWrite(PIN_DATA, outVal ? HIGH : LOW);
}

// Slave Break: ściągnięcie DATA w dół na 4ms w fazie idle magistrali
void issueSlaveBreak() {
    noInterrupts();
    pinMode(PIN_DATA, OUTPUT);
    digitalWrite(PIN_DATA, LOW); 
    delayMicroseconds(4000); 
    pinMode(PIN_DATA, INPUT);
    interrupts();
    lastClockTime = micros();
}

// Przerwanie obsługujące odbiór i nadawanie pojedynczych bitów
void IRAM_ATTR onClockEvent() {
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

// Oblicz checksum medium-command (bajty cmd1 ma bit 7 set = medium/long)
// Dla short: checksum = sum(RAD..CMD2), 6 bajtów total [RAD TAD CMD1 CMD2 PARITY 00]
// Dla medium: checksum po 4 extra data bajtach
void sendShortResponse(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2) {
    uint8_t pkt[6];
    pkt[0] = rad;
    pkt[1] = tad;
    pkt[2] = cmd1;
    pkt[3] = cmd2;
    pkt[4] = rad + tad + cmd1 + cmd2; // Parity
    pkt[5] = 0x00; // End byte
    
    noInterrupts();
    for(int i = 0; i < 6; i++) txBuffer[i] = pkt[i];
    txLength = 6;
    txIndex = 0;
    txBitIndex = 0;
    isAnswering = true;
    pinMode(PIN_DATA, OUTPUT);
    interrupts();
}

// Medium response: RAD TAD CMD1 CMD2 PARITY1 D1 D2 D3 D4 PARITY2 END
void sendMediumResponse(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2,
                        uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4) {
    uint8_t pkt[11];
    pkt[0] = rad;
    pkt[1] = tad;
    pkt[2] = cmd1;
    pkt[3] = cmd2;
    pkt[4] = (uint8_t)(rad + tad + cmd1 + cmd2); // Parity1
    pkt[5] = d1;
    pkt[6] = d2;
    pkt[7] = d3;
    pkt[8] = d4;
    pkt[9] = (uint8_t)(rad + tad + cmd1 + cmd2 + d1 + d2 + d3 + d4); // Parity2
    pkt[10] = 0x00;
    
    noInterrupts();
    for(int i = 0; i < 11; i++) txBuffer[i] = pkt[i];
    txLength = 11;
    txIndex = 0;
    txBitIndex = 0;
    isAnswering = true;
    pinMode(PIN_DATA, OUTPUT);
    interrupts();
}

// Long response (16 bajtów): RAD TAD CMD1 CMD2 P1 D1-D4 D5-D9 P2 END
void sendLongResponse(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2,
                      uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4,
                      uint8_t d5, uint8_t d6, uint8_t d7, uint8_t d8, uint8_t d9) {
    uint8_t pkt[16];
    pkt[0] = rad;
    pkt[1] = tad;
    pkt[2] = cmd1;
    pkt[3] = cmd2;
    uint8_t sum = rad + tad + cmd1 + cmd2;
    pkt[4] = sum; // Parity1
    pkt[5] = d1;
    pkt[6] = d2;
    pkt[7] = d3;
    pkt[8] = d4;
    pkt[9] = d5;
    pkt[10] = d6;
    pkt[11] = d7;
    pkt[12] = d8;
    pkt[13] = d9;
    sum += d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9;
    pkt[14] = sum; // Parity2
    pkt[15] = 0x00;
    
    noInterrupts();
    for(int i = 0; i < 16; i++) txBuffer[i] = pkt[i];
    txLength = 16;
    txIndex = 0;
    txBitIndex = 0;
    isAnswering = true;
    pinMode(PIN_DATA, OUTPUT);
    interrupts();
}

// Wysłanie surowego pakietu (z gotowymi checksumami)
void sendRawPacket(const uint8_t* data, int len) {
    if(len > 64) return;
    
    noInterrupts();
    for(int i = 0; i < len; i++) txBuffer[i] = data[i];
    txLength = len;
    txIndex = 0;
    txBitIndex = 0;
    isAnswering = true;
    pinMode(PIN_DATA, OUTPUT);
    interrupts();
}

void setup() {
    Serial.begin(115200);
    Serial.println("--- Sony UniLink EMULATOR (10-CD) v3 ---");
    Serial.println("Oczekuje na radio (Stan C0 - Init)...");
    
    pinMode(PIN_BUS_ON, INPUT);
    pinMode(PIN_CLOCK, INPUT);
    pinMode(PIN_DATA, INPUT);
    
    attachInterrupt(digitalPinToInterrupt(PIN_CLOCK), onClockEvent, CLOCK_EDGE);
    lastSecondTick = millis();
    lastPingTime = millis();
    initWaitTime = 0;
}

void enterSeekMode() {
    cdState = 0x40; // Loading
    seekStartTime = millis();
    playSeconds = 0;
    playMinutes = 0;
    needDisplayUpdate = true;
}

void processIncomingPacket(uint8_t* buf, int len) {
    if (len < 6) return;
    
    lastPingTime = millis();
    
    uint8_t rad = buf[0];
    uint8_t tad = buf[1];
    uint8_t op1 = buf[2];
    uint8_t op2 = buf[3];
    
    // ===== 1. ANYONE? (18 10 01 02) - Broadcast discovery =====
    if (rad == 0x18 && tad == 0x10 && op1 == 0x01 && op2 == 0x02) {
        if (!deviceAllocated) {
            // 10 30 8C D0 | 9C | 04 AC 1F A3 | 0E 00
            const uint8_t attr[] = {0x10, 0x30, 0x8C, 0xD0, 0x9C, 0x04, 0xAC, 0x1F, 0xA3, 0x0E, 0x00};
            sendRawPacket(attr, sizeof(attr));
            Serial.println(">> Odpowiadam na ANYONE? z atrybutami");
        }
    }
    
    // ===== 2. ADDRESS APPOINT (31 10 02 21) =====
    else if (rad == 0x31 && tad == 0x10 && op1 == 0x02 && op2 == 0x21) {
        deviceAllocated = true; 
        cdState = 0xC0;
        initWaitTime = 0;
        // Potwierdzenie: 10 31 8C D0 | 9D | 04 AC 1F A3 | 0F 00
        const uint8_t status[] = {0x10, 0x31, 0x8C, 0xD0, 0x9D, 0x04, 0xAC, 0x1F, 0xA3, 0x0F, 0x00};
        sendRawPacket(status, sizeof(status));
        Serial.println(">> Adres przydzielony! deviceAllocated=true");
    }
    
    // ===== 3. SLAVE POLL - Who wants display? (18 10 01 15) =====
    // Gdy radio widzi Slave Break, pyta kto chce aktualizować wyświetlacz
    else if (rad == 0x18 && tad == 0x10 && op1 == 0x01 && op2 == 0x15) {
        // Odpowiedz: 10 18 82 00 | AA | 10 00 00 00 | BA 00
        // Bajt D1 = 0x10 oznacza "urządzenie 10 (my/CD changer) chce ekran"
        sendMediumResponse(0x10, 0x18, 0x82, 0x00, 0x10, 0x00, 0x00, 0x00);
        Serial.println(">> Odpowiadam na 01 15 (Slave Poll): chcę ekran!");
    }
    
    // ===== 4. PING - Status query (31 10 01 12) =====
    // WAŻNE: Odpowiadamy TYLKO na zapytania do NAS (tad == 0x10), 
    // a odpowiedź idzie na adres 0x31 (nasz adres przydzielony przez radio)
    else if (rad == 0x31 && tad == 0x10 && op1 == 0x01 && op2 == 0x12) {
        if (cdState == 0xC0 && initWaitTime == 0) {
            initWaitTime = millis();
        }
        
        // Prawdziwa zmieniarka odpowiada: 10 31 00 <state> <parity> 00
        // np. 10 31 00 C0 01 00 (init), 10 31 00 80 C1 00 (idle), 10 31 00 00 41 00 (play)
        sendShortResponse(0x10, 0x31, 0x00, cdState);
        
        // Debug
        Serial.printf(">> PING odpowiedź: stan=0x%02X\n", cdState);
    }
    
    // ===== 5. UPDATE DISPLAY (31 10 01 13) =====
    // TYLKO gdy tad == 0x10 (pytanie skierowane do nas przez main controller)
    // NIE odpowiadamy na 31 14 01 13 - to jest pytanie do display processora (0x14), nie do nas!
    else if (rad == 0x31 && tad == 0x10 && op1 == 0x01 && op2 == 0x13) {
        needDisplayUpdate = false; // Obsłużone
        
        if (cdState == 0x00) {
            // === PLAYING: Wysyłamy sekundnik ===
            // Format z prawdziwej zmieniarki: 70 31 90 00 31 F1 F0 <secBCD> <discNibble>C <parity> 00
            // Przykłady z logów:
            //   70 31 90 00 31 F1 F0 01 6C 7F 00  (disc 6, track 1, 1 sek)
            //   70 31 90 00 31 F1 F0 04 6C 84 00  (disc 6, track 1, 4 sek)
            //   70 31 90 00 31 F1 F0 15 6E 95 00  (disc 6, track 1, 15 sek - BCD!)
            
            uint8_t secBCD = ((playSeconds / 10) << 4) | (playSeconds % 10);
            
            // discNibble: górny nibble = numer dysku, dolny nibble = marker (widzimy "6C", "1C" itd.)
            uint8_t discNibble = ((currentDisk & 0x0F) << 4) | 0x0C;
            // Track info: F<track_tens> w Data2, F<track_units> w Data1
            // Z logów: 0x31=const, F1=track1 jedności, F0=track1 dziesiątki
            uint8_t trackOnes = 0xF0 | (currentTrack % 10);    // np. F1 for track 1
            uint8_t trackTens = 0xF0 | ((currentTrack / 10) % 10); // np. F0 for track <10
            
            // Medium response: RAD=70 TAD=31 CMD1=90 CMD2=00 P1(auto) D1=trackOnes D2=trackTens D3=secBCD D4=discNibble P2(auto) END=00
            // Z logów: 70 31 90 00 [31] F1 F0 01 6C [7F] 00
            //   P1 = (0x70+0x31+0x90+0x00) = 0x31 (auto)
            //   D1=F1, D2=F0, D3=01(sec), D4=6C(disc6+markerC)
            //   P2 = 0x7F (auto)
            sendMediumResponse(0x70, 0x31, 0x90, 0x00, 
                             trackOnes, trackTens, secBCD, discNibble);
            
            Serial.printf(">> DISPLAY: Playing CD%d TR%d %02d:%02d sec\n", 
                         currentDisk, currentTrack, playMinutes, playSeconds);
        }
        else if (cdState == 0x40 || cdState == 0x20) {
            // === LOADING/SEEKING ===
            // Long response: 70 31 C0 40 | P1 | D1-D9 | P2 | 00
            // Z logów: 70 31 C0 40 A1 00 00 00 00 00 F1 F0 00 50 D2 00 (disc 5, track 1)
            uint8_t trackOnes = 0xF0 | (currentTrack % 10);
            uint8_t trackTens = 0xF0 | ((currentTrack / 10) % 10);
            uint8_t discNibble = ((currentDisk & 0x0F) << 4);
            
            sendLongResponse(0x70, 0x31, 0xC0, 0x40,
                           0x00, 0x00, 0x00, 0x00, 0x00,
                           trackOnes, trackTens, 0x00, discNibble);
            
            Serial.printf(">> DISPLAY: Loading/Seeking CD%d TR%d\n", currentDisk, currentTrack);
        }
        else {
            // C0 (init) lub 0x80 (idle) - odpowiadamy czymś prostym
            // Z logów widać "8E" pakiety podczas różnych faz init
            // Na starcie prawdziwa odpowiadała: 70 31 8E F0 1F F5 00 00 10 24 00
            // Spróbujmy medium z pustą informacją
            sendMediumResponse(0x70, 0x31, 0x8E, 0xC0, 0x00, 0x00, 0x00, 0x40);
            
            Serial.printf(">> DISPLAY: Init/Idle state 0x%02X\n", cdState);
        }
    }
    
    // ===== 6. WAKE UP / PLAY command (31 10 20 00) =====
    else if (rad == 0x31 && tad == 0x10 && op1 == 0x20 && op2 == 0x00) {
        Serial.println(">> PLAY command received!");
        enterSeekMode();
    }
    
    // ===== 7. Button commands from front panel (31 11 XX XX) =====
    else if (rad == 0x31 && tad == 0x11) {
        if (op1 == 0x26 && op2 == 0x10) {
            currentTrack++;
            if(currentTrack > 99) currentTrack = 1;
            enterSeekMode();
            Serial.printf(">> NEXT TRACK: %d\n", currentTrack);
        } 
        else if (op1 == 0x27 && op2 == 0x10) {
            currentTrack--;
            if(currentTrack < 1) currentTrack = 99;
            enterSeekMode();
            Serial.printf(">> PREV TRACK: %d\n", currentTrack);
        }
        else if (op1 == 0x28) {
            currentDisk++;
            if(currentDisk > 10) currentDisk = 1;
            currentTrack = 1;
            enterSeekMode();
            Serial.printf(">> NEXT DISC: %d\n", currentDisk);
        }
        else if (op1 == 0x29) {
            currentDisk--;
            if(currentDisk < 1) currentDisk = 10;
            currentTrack = 1;
            enterSeekMode();
            Serial.printf(">> PREV DISC: %d\n", currentDisk);
        }
    }
}

void loop() {
    unsigned long now = millis();

    // Timeout: radio zniknęło
    if (deviceAllocated && (now - lastPingTime > 5000)) {
        deviceAllocated = false;
        cdState = 0xC0;
        initWaitTime = 0;
        wantSlaveBreak = false;
        needDisplayUpdate = false;
        Serial.println("--- Radio timeout (5s). Reset do C0 ---");
    }

    // Maszyna stanów: C0 -> 80 (po 800ms od pierwszego PINGa)
    if (cdState == 0xC0 && deviceAllocated && initWaitTime != 0 && (now - initWaitTime > 800)) {
        cdState = 0x80;
        Serial.println(">>> C0 -> 80 (Idle)");
    }

    // 0x40 -> 0x20 -> 0x00 (Loading -> Seeking -> Playing)
    if (cdState == 0x40 && (now - seekStartTime > 800)) {
        cdState = 0x20;
        seekStartTime = millis();
        needDisplayUpdate = true;
        Serial.println(">>> 40 -> 20 (Seeking)");
    }
    else if (cdState == 0x20 && (now - seekStartTime > 800)) {
        cdState = 0x00;
        needDisplayUpdate = true;
        lastSecondTick = millis();
        playSeconds = 0;
        playMinutes = 0;
        Serial.println(">>> 20 -> 00 (Playing!)");
    }

    // Sekundnik
    if (cdState == 0x00 && now - lastSecondTick >= 1000) {
        playSeconds++;
        if (playSeconds >= 60) {
            playSeconds = 0;
            playMinutes++;
            if (playMinutes >= 100) playMinutes = 0;
        }
        lastSecondTick = millis();
        needDisplayUpdate = true;
    }
    
    // Slave Break: zgłaszamy się tylko gdy mamy coś nowego do pokazania
    // i na magistrali jest cicho (> 8ms od ostatniego clocka)
    noInterrupts();
    bool silence = (micros() - lastClockTime > 5000); 
    bool silToBreak = (micros() - lastClockTime > 8000); 
    int count = rxIndex;
    interrupts();
    
    if (needDisplayUpdate && silToBreak && deviceAllocated && cdState == 0x00) {
        if (millis() - lastBreakTime > 800) { // Max 1 break na ~sekundę, jak prawdziwa zmieniarka
            issueSlaveBreak();
            lastBreakTime = millis();
            // NIE kasujemy needDisplayUpdate — skasuje się gdy radio odpyta 01 13
        }
    }
    
    // Przetwarzanie odebranych pakietów
    if (silence && count > 0) {
        uint8_t tempBuf[RX_BUFFER_SIZE];
        noInterrupts();
        for(int i = 0; i < count; i++) tempBuf[i] = rxBuffer[i];
        rxIndex = 0;
        rxBitIndex = 0;
        interrupts();
        
        // Debug output
        Serial.print("RX: ");
        for(int i = 0; i < count; i++) {
             if (tempBuf[i] < 0x10) Serial.print("0");
             Serial.print(tempBuf[i], HEX); Serial.print(" ");
        }
        Serial.println();
        
        processIncomingPacket(tempBuf, count);
    }
}
