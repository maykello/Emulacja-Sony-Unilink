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

// Hardkodowana długość każdej piosenki (s). Po upływie -> auto next track.
const uint16_t SONG_DURATION_SEC = 70; // 1:10

// Maksymalne wartości
const uint8_t MAX_TRACK_PER_DISC = 99;
const uint8_t MAX_DISC = 10;

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

// --- Strojenie czasów ---
const unsigned long LOAD_DURATION_MS  = 50;   // 0x40 -> 0x20 (krótki, by uniknąć migania LOAD)
const unsigned long SEEK_DURATION_MS  = 50;   // 0x20 -> 0x00 (krótki, by uniknąć migania LOAD)
const unsigned long DISC_LOAD_MS      = 200;  // dłuższy seek przy zmianie płyty (bardziej realistyczne)
const unsigned long BREAK_INTERVAL_MS = 150;  // odstęp między Slave Breakami
const unsigned long BREAK_SILENCE_US  = 6000; // ile cisza musi trwać aby zaryzykować Break
const unsigned long BREAK_HOLD_US     = 2500; // jak długo trzymać DATA LOW jako Slave Break

// Slave Break - flagi
bool wantSlaveBreak = false;
unsigned long lastBreakTime = 0;
bool needDisplayUpdate = false;

// --- BUS_ON tracking ---
// Stan magistrali z pinu BUS_ON. Prawdziwa zmieniarka jest zasilana z magistrali,
// wiec gdy radio wylacza zasilanie magistrali (BUS=0) - zmieniarka jest WYLACZONA
// i NIE odpowiada na nic. CDX-M670 wykorzystuje fazy BUS=0/1 do dyskryminacji
// urzadzen wewnetrznych (CD radia, addr 0x3B) od zewnetrznych zmieniarek (0x31).
// Nasz ESP32 jest zasilany z USB i bez tego pinu odpowiadalby zawsze - co radio
// interpretuje jako konflikt urzadzenia wewnetrznego z zewnetrznym i wpada
// w nieskonczona petle SYSTEM RESET. Sprawdzamy wiec BUS_ON i symulujemy
// brak zasilania gdy BUS=0.
bool busPoweredLast = false;

// --- CDX-M670 specific tracking ---
// Rozroznienie pomiedzy MEX-BT3800u i CDX-M670:
//   MEX:      pelne discovery z jednym ANYONE? -> APPOINT (op2=0x21) -> dziala.
//   CDX-M670: 2-fazowe discovery. Faza preliminary (op2=0x11/0x12) jest dla
//             wewnetrznych urzadzen radia (jego wlasne CD pod 0x3B i kontroler
//             pod 0x71). Prawdziwa zmieniarka odpowiada DOPIERO w fazie glownej,
//             gdy radio wysyla "puste" ANYONE? po cyklu BUS_ON. Jesli odpowiemy
//             w fazie preliminary, zostaniemy przydzieleni w slot op2=0x12 zamiast
//             wlasciwego op2=0x14, co powoduje nieskonczona petle RESET.
//
// Markery CDX-M670 (nie wystepuja na MEX):
//   - 3B 10 02 11 = appoint dla wewnetrznego CD radia
//   - DB 10 02 12 = appoint dla wewnetrznego pomocniczego
// Po zobaczeniu DB nastepuje OKNO PRELIMINARY (~250ms), w ktorym IGNORUJEMY
// ANYONE?. Po tym oknie reagujemy normalnie.
bool isCdxM670 = false;
unsigned long lastPreliminaryTime = 0;
const unsigned long PRELIMINARY_WINDOW_MS = 250;
int anyoneIgnoredCount = 0;   // tylko do logowania

// Licznik resetow w petli - po kilku resetach probujemy bardziej agresywnie
// odpowiedziec na 01 01 / 01 11, zeby sprowokowac radio do cyklu BUS_ON.
int resetLoopCount = 0;

void setTxData(bool bitVal) {
    bool outVal = INVERT_DATA ? !bitVal : bitVal;
    digitalWrite(PIN_DATA, outVal ? HIGH : LOW);
}

// Slave Break: ściągnięcie DATA w dół na chwilę w fazie idle magistrali.
// Trzymamy DATA krótko i NIE wyłączamy przerwań na cały czas — ESP32 przy
// 4ms noInterrupts() gubi clocki od radia i potrafi rozsypać kolejny pakiet,
// co radio interpretuje jako kolizję i wysyła system-reset (01 00).
void issueSlaveBreak() {
    pinMode(PIN_DATA, OUTPUT);
    digitalWrite(PIN_DATA, LOW);
    delayMicroseconds(BREAK_HOLD_US);
    pinMode(PIN_DATA, INPUT);
    // Reset stanu odbiornika - uniknij interpretacji "ogona" naszego break-a
    // jako początku bajtu od radia
    noInterrupts();
    rxBitIndex = 0;
    rxIncomingByte = 0;
    lastClockTime = micros();
    interrupts();
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
    Serial.println("--- Sony UniLink EMULATOR (10-CD) v8 ---");
    Serial.println("Obsluga: MEX-BT3800u + CDX-M670 (poprawne 01 15: cmd2=01/04/05 wg stanu)");
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

    // ===== 0a. Detekcja CDX-M670 + okno preliminary =====
    // 3B 10 02 11 = appoint wewnetrznego CD radia (TYLKO CDX-M670 to wysyla)
    // DB 10 02 12 = appoint wewnetrznego pomocniczego (TYLKO CDX-M670)
    // Po tych pakietach zaczynamy ignorowac ANYONE? na ~250ms zeby radio
    // dokonczylo preliminary discovery bez nas (tak zachowuje sie prawdziwa
    // zmieniarka, ktora jest wtedy bez zasilania).
    if (rad == 0x3B && tad == 0x10 && op1 == 0x02 && op2 == 0x11) {
        if (!isCdxM670) {
            isCdxM670 = true;
            Serial.println("== Wykryto CDX-M670 (widziano 3B 10 02 11) ==");
        }
        lastPreliminaryTime = millis();
        return;
    }
    if (rad == 0xDB && tad == 0x10 && op1 == 0x02 && op2 == 0x12) {
        lastPreliminaryTime = millis();
        return;
    }

    // ===== 1. ANYONE? (18 10 01 02) - Broadcast discovery =====
    // Radio wysyła to wielokrotnie po starcie, żeby znaleźć WSZYSTKIE urządzenia.
    // Odpowiadamy TYLKO gdy jeszcze nie mamy adresu — w przeciwnym razie radio
    // przydzieli nam drugi/trzeci adres myśląc że jesteśmy nowym urządzeniem.
    if (rad == 0x18 && tad == 0x10 && op1 == 0x01 && op2 == 0x02) {
        if (!deviceAllocated) {
            // CDX-M670: ignoruj ANYONE? w oknie preliminary - to faza dla
            // wewnetrznych urzadzen radia, nie dla nas. Inaczej wpadamy w slot
            // op2=0x12 zamiast wlasciwego op2=0x14 -> nieskonczona petla RESET.
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
            sendRawPacket(attr, sizeof(attr));
            Serial.println(">> Odpowiadam na ANYONE? z atrybutami");
        }
    }

    // ===== 1a. UNAPPOINTED CHANGER QUERY (18 10 01 11) i (18 10 01 01) =====
    // Radio wysyla to zeby sprawdzic czy jest nieprzydzielona zmieniarka.
    // Prawdziwa zmieniarka odpowiada wtedy magicznym pakietem 10 18 04 00 2C 00
    // (patrz log sniff zmieniarki linia 164). To wywoluje u radia SYSTEM RESET
    // i nastepnie cykl BUS_ON, ktory inicjuje WLASCIWE discovery z op2=0x14.
    //
    // Odpowiadamy TYLKO gdy nie jestesmy przydzieleni. W trybie CDX-M670
    // odpowiadamy tez na op2=0x01 (CDX-M670 wysyla 01 01 zamiast 01 11).
    else if (rad == 0x18 && tad == 0x10 && op1 == 0x01 &&
             (op2 == 0x11 || (isCdxM670 && op2 == 0x01))) {
        if (!deviceAllocated) {
            const uint8_t magic[] = {0x10, 0x18, 0x04, 0x00, 0x2C, 0x00};
            sendRawPacket(magic, sizeof(magic));
            Serial.printf(">> Odpowiadam na 01 %02X (nieprzydzielona zmieniarka): magic 10 18 04 00\n", op2);
        }
    }

    // ===== 1b. SYSTEM RESET (18 10 01 00) =====
    // Radio wysyła to gdy chce przerwać sesję i zacząć discovery od nowa.
    // Wtedy musimy zapomnieć stary adres i ponownie odpowiadać na ANYONE?.
    else if (rad == 0x18 && tad == 0x10 && op1 == 0x01 && op2 == 0x00) {
        resetLoopCount++;
        if (deviceAllocated) {
            Serial.printf(">> Radio system reset (#%d)! Reset deviceAllocated.\n", resetLoopCount);
            deviceAllocated = false;
            cdState = 0xC0;
            initWaitTime = 0;
            needDisplayUpdate = false;
        } else {
            Serial.printf(">> Radio system reset (#%d) (juz bylem nieprzydzielony)\n", resetLoopCount);
        }
        // Po resecie zaczynamy nowy cykl preliminary - kasujemy znacznik czasu,
        // bedzie ustawiony ponownie gdy zobaczymy 3B/DB w nowym cyklu.
        lastPreliminaryTime = 0;
        anyoneIgnoredCount = 0;
    }
    
    // ===== 2. ADDRESS APPOINT (31 10 02 XX) =====
    // Radio przydziela nam adres 0x31. Bajt op2 to numer sesji/sekwencji
    // i ROZNI SIE miedzy radiami:
    //   - Sony MEX-BT3800u: 31 10 02 21 (op2=0x21)
    //   - Sony CDX-M670:    31 10 02 14 (op2=0x14)
    // Dlatego akceptujemy KAZDE op2 - liczy sie tylko ze radio
    // przydziela adres 0x31 do klasy 0x10 (CD changer) komenda 0x02.
    else if (rad == 0x31 && tad == 0x10 && op1 == 0x02) {
        deviceAllocated = true;
        cdState = 0xC0;
        initWaitTime = 0;
        // ATRYBUTY ZGODNE Z PRAWDZIWA ZMIENIARKA (CDX-M670 sniff):
        //   10 31 8C D0 | 9D | 04 A8 1F A3 | 0B 00
        // Vs poprzednia: D2=A8 (bylo AC). Patrz komentarz przy ANYONE? wyzej.
        const uint8_t status[] = {0x10, 0x31, 0x8C, 0xD0, 0x9D, 0x04, 0xA8, 0x1F, 0xA3, 0x0B, 0x00};
        sendRawPacket(status, sizeof(status));
        Serial.printf(">> Adres przydzielony (op2=0x%02X)! deviceAllocated=true\n", op2);
    }
    
    // ===== 3. SLAVE POLL - Who wants display? (18 10 01 15) =====
    // Gdy radio widzi Slave Break, pyta kto chce aktualizować wyświetlacz.
    // Format odpowiedzi (zgodny z prawdziwa zmieniarka, sniff CDX-M670):
    //   10 18 82 XX | PAR1 | 00 00 00 00 | PAR2 00
    // gdzie XX (cmd2) = TYP zadanego ekranu:
    //   0x00 = "nic, nie pytaj mnie o ekran"   <-- BLAD! Tego radio nie obsluguje
    //   0x01 = "ekran startowy / idle"         <-- prawdziwa zmieniarka w stanie Init/Idle
    //   0x04 = "ekran odtwarzania (track/time)" <-- prawdziwa zmieniarka w stanie Play
    //   0x05 = "ekran przejsciowy (loading/seeking)"
    // Bajty D1-D4 sa zawsze 0x00 (informacja jest w cmd2).
    //
    // Poprzednia wersja wysylala cmd2=0x00 + D1=0x10, co radio interpretowalo
    // jako "nic nie chce" -> nigdy nie wysylalo nam 01 13 (UPDATE DISPLAY) -> nie
    // dalo sie wejsc na zmieniarke.
    else if (rad == 0x18 && tad == 0x10 && op1 == 0x01 && op2 == 0x15) {
        uint8_t displayType;
        if (cdState == 0x00) {
            displayType = 0x04;  // Playing - chce pelny ekran z track/czas
        } else if (cdState == 0x40 || cdState == 0x20) {
            displayType = 0x05;  // Loading/Seeking - ekran przejsciowy
        } else {
            displayType = 0x01;  // Init/Idle - ekran startowy
        }
        sendMediumResponse(0x10, 0x18, 0x82, displayType, 0x00, 0x00, 0x00, 0x00);
        Serial.printf(">> Odpowiadam na 01 15 (Slave Poll): chce ekran typ 0x%02X (stan=0x%02X)\n",
                      displayType, cdState);
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

        // ZAWSZE wysyłamy pakiet "Playing" (90 00) niezależnie od cdState.
        // Stan loading/seek/play radio bierze z PING-a (01 12), a tutaj zawsze
        // chcemy żeby ekran pokazywał aktualny utwór i czas. Wysyłanie pakietu
        // Loading (C0 40) powodowało miganie "LOAD" przy szybkich zmianach tracka.
        //
        // Format: 70 31 90 00 P1 D1 D2 D3 D4 P2 00
        //   D1 = TRACK BCD (z paddingiem 0xF dla pojedynczych cyfr: F1=01 ... 99=99)
        //   D2 = MINUTY BCD (z paddingiem 0xF)
        //   D3 = SEKUNDY BCD (00..59)
        //   D4 = górny nibble = numer dysku (1..A), dolny nibble = 0xC marker
        uint8_t secBCD = ((playSeconds / 10) << 4) | (playSeconds % 10);

        uint8_t trackBCD;
        if (currentTrack < 10) {
            trackBCD = 0xF0 | currentTrack;
        } else {
            trackBCD = ((currentTrack / 10) << 4) | (currentTrack % 10);
        }

        uint8_t minBCD;
        if (playMinutes < 10) {
            minBCD = 0xF0 | playMinutes;
        } else {
            minBCD = ((playMinutes / 10) << 4) | (playMinutes % 10);
        }

        uint8_t discNibble = ((currentDisk & 0x0F) << 4) | 0x0C;

        sendMediumResponse(0x70, 0x31, 0x90, 0x00,
                         trackBCD, minBCD, secBCD, discNibble);

        Serial.printf(">> DISPLAY: CD%d TR%d %02d:%02d (state=0x%02X)\n",
                     currentDisk, currentTrack, playMinutes, playSeconds, cdState);
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
            if(currentTrack > MAX_TRACK_PER_DISC) currentTrack = 1;
            enterSeekMode();
            Serial.printf(">> NEXT TRACK: %d\n", currentTrack);
        } 
        else if (op1 == 0x27 && op2 == 0x10) {
            if (currentTrack <= 1) currentTrack = MAX_TRACK_PER_DISC;
            else currentTrack--;
            enterSeekMode();
            Serial.printf(">> PREV TRACK: %d\n", currentTrack);
        }
        else if (op1 == 0x28) {
            currentDisk++;
            if(currentDisk > MAX_DISC) currentDisk = 1;
            currentTrack = 1;
            enterSeekMode();
            Serial.printf(">> NEXT DISC: %d\n", currentDisk);
        }
        else if (op1 == 0x29) {
            if (currentDisk <= 1) currentDisk = MAX_DISC;
            else currentDisk--;
            currentTrack = 1;
            enterSeekMode();
            Serial.printf(">> PREV DISC: %d\n", currentDisk);
        }
    }
}

void loop() {
    unsigned long now = millis();

    // ===== BUS_ON detection =====
    // Symulujemy zachowanie zmieniarki zasilanej z magistrali: gdy BUS=0,
    // jestesmy "wylaczeni" (nie odpowiadamy, gubimy stan). To NIEZBEDNE dla
    // CDX-M670, ktore w fazie BUS=0 prowadzi wewnetrzna komunikacje (do
    // wlasnego CD-radia) i jakakolwiek odpowiedz zewnetrznego urzadzenia
    // w tej fazie powoduje konflikt i petle resetow.
    bool busPowered = (digitalRead(PIN_BUS_ON) == HIGH);
    if (busPowered != busPoweredLast) {
        busPoweredLast = busPowered;
        Serial.printf("=== BUS_ON = %d ===\n", busPowered ? 1 : 0);
        if (!busPowered) {
            // Magistrala wylaczona - resetujemy caly stan tak jakby zasilanie znikalo
            deviceAllocated = false;
            cdState = 0xC0;
            initWaitTime = 0;
            wantSlaveBreak = false;
            needDisplayUpdate = false;
            // Skasuj tez bufor odbiorczy - bajty z BUS=0 sa "obce"
            noInterrupts();
            rxIndex = 0;
            rxBitIndex = 0;
            rxIncomingByte = 0;
            interrupts();
            // Reset markerow preliminary (ale NIE isCdxM670 - to zostaje na zawsze
            // po wykryciu, zeby przezyc cykl BUS_ON)
            lastPreliminaryTime = 0;
            anyoneIgnoredCount = 0;
            resetLoopCount = 0;
        }
    }

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
    if (cdState == 0x40 && (now - seekStartTime > LOAD_DURATION_MS)) {
        cdState = 0x20;
        seekStartTime = millis();
        needDisplayUpdate = true;
        Serial.println(">>> 40 -> 20 (Seeking)");
    }
    else if (cdState == 0x20 && (now - seekStartTime > SEEK_DURATION_MS)) {
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

        // Auto-next: po SONG_DURATION_SEC przeskakujemy na kolejny utwór.
        // Dojechaliśmy do końca płyty -> kolejna płyta. Z disc 10 wracamy na 1.
        uint16_t totalSec = (uint16_t)playMinutes * 60 + playSeconds;
        if (totalSec >= SONG_DURATION_SEC) {
            currentTrack++;
            if (currentTrack > MAX_TRACK_PER_DISC) {
                currentTrack = 1;
                currentDisk++;
                if (currentDisk > MAX_DISC) currentDisk = 1;
            }
            enterSeekMode();
            Serial.printf(">> AUTO-NEXT: CD%d TR%d\n", currentDisk, currentTrack);
        }
    }
    
    // Slave Break: zgłaszamy się gdy mamy coś nowego do pokazania
    // i na magistrali jest cicho. Tylko w stanie 0x00 (Playing), nie wysyłamy
    // pakietu Loading który powodowałby miganie LOAD.
    noInterrupts();
    bool silence = (micros() - lastClockTime > 5000);
    bool silToBreak = (micros() - lastClockTime > BREAK_SILENCE_US);
    bool busy = isAnswering; // nie przerywaj własnej odpowiedzi
    int count = rxIndex;
    interrupts();

    bool stateAllowsBreak = (cdState == 0x00);
    if (needDisplayUpdate && silToBreak && !busy && deviceAllocated && stateAllowsBreak && busPowered) {
        if (millis() - lastBreakTime > BREAK_INTERVAL_MS) {
            issueSlaveBreak();
            lastBreakTime = millis();
            // NIE kasujemy needDisplayUpdate — skasuje się gdy radio odpyta 01 13
        }
    }
    
    // Przetwarzanie odebranych pakietów - TYLKO gdy BUS_ON=1.
    // Gdy BUS=0, pochlaniamy bajty (czyscimy bufor) ale ich NIE przetwarzamy
    // i NIE odpowiadamy. To kluczowe dla wspolpracy z CDX-M670.
    if (silence && count > 0) {
        uint8_t tempBuf[RX_BUFFER_SIZE];
        noInterrupts();
        for(int i = 0; i < count; i++) tempBuf[i] = rxBuffer[i];
        rxIndex = 0;
        rxBitIndex = 0;
        interrupts();

        if (busPowered) {
            // Debug output
            Serial.print("RX: ");
            for(int i = 0; i < count; i++) {
                 if (tempBuf[i] < 0x10) Serial.print("0");
                 Serial.print(tempBuf[i], HEX); Serial.print(" ");
            }
            Serial.println();

            processIncomingPacket(tempBuf, count);
        }
        // gdy !busPowered: cisza, tylko opuszczamy bufor
    }
}
