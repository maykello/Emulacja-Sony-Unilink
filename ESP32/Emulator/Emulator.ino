#include <Arduino.h>
#include <Preferences.h>
#include "AudioPlayer.h"

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

// Maksymalne wartości (fallback gdy SD nie zamontowana)
const uint8_t MAX_TRACK_PER_DISC = 99;
const uint8_t MAX_DISC = 10;

// === Pamiec nieulotna (NVS) — zapamietuje ostatnio odtwarzany utwor ===
// Dzieki temu po wylaczeniu radia (BUS=0) i ponownym wlaczeniu emulator
// wznawia od tej samej plyty/utworu, ktory gral ostatnio.
Preferences prefs;
const char* PREFS_NAMESPACE = "unilink";
uint8_t lastSavedDisk = 0;
uint8_t lastSavedTrack = 0;

void persistLastTrack() {
    // Zapisujemy tylko gdy cos sie zmienilo (oszczedzamy zywotnosc NVS).
    if (currentDisk == lastSavedDisk && currentTrack == lastSavedTrack) return;
    prefs.putUChar("disk", currentDisk);
    prefs.putUChar("track", currentTrack);
    lastSavedDisk = currentDisk;
    lastSavedTrack = currentTrack;
    Serial.printf("[NVS] Zapamietano ostatni utwor: CD%d TR%d\n", currentDisk, currentTrack);
}

void loadLastTrack() {
    uint8_t d = prefs.getUChar("disk", 1);
    uint8_t t = prefs.getUChar("track", 1);
    if (d < 1 || d > MAX_DISC) d = 1;
    if (t < 1) t = 1;
    currentDisk = d;
    currentTrack = t;
    lastSavedDisk = d;
    lastSavedTrack = t;
    Serial.printf("[NVS] Wczytano ostatni utwor: CD%d TR%d\n", currentDisk, currentTrack);
}

// === Bus Audio In (routing dzwieku) — tylko diagnostyka ===
// Analiza sniffu pokazala, ze komenda 18 10 87 z magistrali to sygnalizacja
// toru audio radia, a NIE polecenie wyciszenia zmieniarki. Prawdziwa zmieniarka
// gra caly czas. Nie trzymamy juz flagi gatujacej DAC (powodowala ciszę).

bool deviceAllocated = false;
unsigned long lastPingTime = 0;

// --- Adres przydzielony przez radio ---
// KLUCZOWE: radio NIE zawsze przydziela 0x31. Po resecie/ponownym discovery
// CDX-M670 potrafi przydzielic kolejny wolny adres z bloku zmieniarek
// (0x31..0x3A). Jesli na sztywno reagujemy tylko na 0x31, ignorujemy taki
// przydzial, dalej odpowiadamy na ANYONE? i radio przydziela 0x32, 0x33...
// az do zawieszenia (petla 30 10 01 0F). Dlatego adoptujemy KAZDY adres,
// ktory radio nam nada, i uzywamy go we wszystkich odpowiedziach.
uint8_t myAddr = 0x31;

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
const unsigned long BREAK_SILENCE_US  = 8000; // wymagana CISZA przed Break.
        // MUSI byc TUZ POWYZEJ przerwy command->ack radia (~6ms w sniffie). Przy
        // 6000us lapalismy sie dokladnie na moment nadejscia ACK -> kolizja ->
        // radio robilo SYSTEM RESET. Przy 8000us ACK juz przyszedl (i wyzerowal
        // licznik ciszy na 6ms), wiec jestesmy w realnym idle. Za duzo (12000)
        // = break sie nie wyzwala, radio nie odpytuje ekranu (czarny ekran).
const unsigned long BREAK_HOLD_US     = 2500; // jak długo trzymać DATA LOW (na tyle długo by radio wykryło Break)

// Slave Break - flagi
bool wantSlaveBreak = false;
unsigned long lastBreakTime = 0;
bool needDisplayUpdate = false;

// --- Ochrona przed kolizja z odpowiedzia wewnetrznych urzadzen radia ---
// Radio odpytuje swoje wewnetrzne urzadzenia (0x3B = CD radia, 0x71 = kontroler),
// ktore odpowiadaja z opoznieniem ~9-12ms. Prog ciszy do Slave Break (8ms) potrafil
// trafic w te luke i zderzyc sie z ich odpowiedzia (uszkodzona ramka -> radio robi
// SYSTEM RESET, co objawia sie restartem utworu po dluzszej grze). Gdy zobaczymy poll
// mastera do INNEGO urzadzenia, blokujemy break na to okno, az tamto zdazy odpowiedziec.
unsigned long suppressBreakUntil = 0;
const unsigned long FOREIGN_POLL_GUARD_MS = 30;

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
    // Ostateczna kontrola ciszy TUZ przed pociagnieciem (z wlaczonymi
    // przerwaniami clock mogl wlasnie przyjsc).
    noInterrupts();
    unsigned long lastClk = lastClockTime;
    bool stillIdle = (micros() - lastClk > BREAK_SILENCE_US);
    interrupts();
    if (!stillIdle) return;  // radio zaczelo nadawac — NIE przerywaj, odpusc break

    pinMode(PIN_DATA, OUTPUT);
    digitalWrite(PIN_DATA, LOW);
    // Trzymaj DATA nisko, ale PRZERWIJ natychmiast gdy radio ruszy z zegarem.
    // Wczesniej trzymalismy na sztywno 2.5ms — jesli radio w tym czasie zaczynalo
    // pakiet, niszczylismy go (smieci typu '11 40 11 25 80') i radio robilo
    // SYSTEM RESET. Teraz puszczamy magistrale po pierwszym zboczu zegara.
    unsigned long t0 = micros();
    while ((micros() - t0) < BREAK_HOLD_US) {
        noInterrupts();
        unsigned long lc = lastClockTime;
        interrupts();
        if (lc != lastClk) break;  // przyszedl clock = radio nadaje -> puszczamy
    }
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
    Serial.println("--- Sony UniLink EMULATOR (10-CD) v9 + AUDIO ---");
    Serial.println("Obsluga: MEX-BT3800u + CDX-M670 + PCM5102A DAC + SD");
    Serial.println("Oczekuje na radio (Stan C0 - Init)...");
    
    pinMode(PIN_BUS_ON, INPUT);
    pinMode(PIN_CLOCK, INPUT);
    pinMode(PIN_DATA, INPUT);
    
    // Pamiec nieulotna — wczytaj ostatnio odtwarzany utwor
    prefs.begin(PREFS_NAMESPACE, false);
    loadLastTrack();
    
    // Inicjalizacja odtwarzacza audio (SD + I2S/PCM5102A)
    if (audioInit()) {
        Serial.println("[Audio] Gotowy do odtwarzania.");
    } else {
        Serial.println("[Audio] UWAGA: Brak karty SD — emulator dziala bez dzwieku.");
    }
    
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
    
    // Zapamietaj wybor w pamieci nieulotnej (przetrwa wylaczenie radia)
    persistLastTrack();
    
    // Rozpocznij odtwarzanie prawdziwego pliku z SD
    if (!audioPlayTrack(currentDisk, currentTrack)) {
        Serial.printf("[Audio] Brak pliku CD%02d/%02d — szukam nastepnego...\n",
                      currentDisk, currentTrack);
    }
}

void processIncomingPacket(uint8_t* buf, int len) {
    if (len < 6) return;
    
    lastPingTime = millis();
    
    uint8_t rad = buf[0];
    uint8_t tad = buf[1];
    uint8_t op1 = buf[2];
    uint8_t op2 = buf[3];

    // ===== Walidacja sumy kontrolnej naglowka =====
    // We WSZYSTKICH poprawnych ramkach Unilink bajt 5 (buf[4], "Parity1") =
    // (RAD+TAD+OP1+OP2) & 0xFF. Przeklamane/kolidujace ramki (np. '11 40 11 25 8F',
    // '31 10 02 24 A8') tej sumy nie spelniaja. Prawdziwa zmieniarka takie
    // pakiety IGNORUJE — my tez musimy, zeby nie reagowac na smieci (np. nie
    // potraktowac '31 10 02 ..' jako falszywego APPOINT) i nie kaskadowac bledu.
    if (buf[4] != (uint8_t)(rad + tad + op1 + op2)) {
        return; // bledny checksum — ignoruj cala ramke
    }

    // ===== Okno ochronne: poll mastera do INNEGO urzadzenia =====
    // tad == 0x10 oznacza ramke OD mastera (radia). Jesli adresatem (rad) jest
    // ktos inny niz my i nie jest to broadcast 0x18, to radio wlasnie odpytalo
    // swoje wewnetrzne urzadzenie (0x3B/0x71/...), ktore za chwile (~9-12ms)
    // odpowie. Wstrzymujemy Slave Break na to okno, zeby nie zderzyc sie z ta
    // odpowiedzia (kolizja psula ramke i wywolywala SYSTEM RESET radia).
    if (tad == 0x10 && rad != myAddr && rad != 0x18) {
        suppressBreakUntil = millis() + FOREIGN_POLL_GUARD_MS;
    }

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

    // ===== 0b. BUS AUDIO IN — diagnostyka routingu dzwieku (18 10 87 ...) =====
    // Radio rozglasza do klasy zmieniarki komende audio. UWAGA: analiza sniffu
    // pokazala, ze gorny nibble op2 to tylko licznik sekwencji (2->6->A->E), a
    // bit0 = stan ("xB"=on, "xA"=off). Prawdziwa zmieniarka NIGDY nie wycisza
    // wlasnego wyjscia audio na podstawie tej komendy — gra caly czas, a radio
    // samo decyduje o swoim torze audio. Dlatego TYLKO logujemy (do diagnostyki)
    // i NIE ruszamy glosnosci DAC. Wczesniejsze wyciszanie powodowalo, ze rutynowe
    // "87 6A" w trakcie grania wyciszalo dzwiek na amen.
    if (rad == 0x18 && tad == 0x10 && op1 == 0x87) {
        bool on = (op2 & 0x01) != 0;
        Serial.printf(">> [Audio Bus] Radio sygnalizuje audio %s (87 %02X) — tylko log\n",
                      on ? "ON" : "OFF", op2);
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
        myAddr = 0x31; // nastepny przydzial moze byc inny — zaadoptujemy go ponownie
        // Po resecie zaczynamy nowy cykl preliminary - kasujemy znacznik czasu,
        // bedzie ustawiony ponownie gdy zobaczymy 3B/DB w nowym cyklu.
        lastPreliminaryTime = 0;
        anyoneIgnoredCount = 0;
    }
    
    // ===== 2. ADDRESS APPOINT (3X 10 02 XX) =====
    // Radio przydziela nam adres z bloku zmieniarek (0x31..0x3A). Bajt op2 to
    // numer sesji/sekwencji i ROZNI SIE miedzy radiami (MEX=0x21, CDX=0x14) —
    // akceptujemy KAZDE op2. Adres tez moze byc rozny niz 0x31 (po resecie radio
    // potrafi nadac 0x32+). Adoptujemy go do myAddr i od tej pory na nim gramy.
    // (0x3B = wewnetrzne CD radia — obsluzone wczesniej z 'return', tu nie wejdzie.)
    else if (rad >= 0x31 && rad <= 0x3A && tad == 0x10 && op1 == 0x02) {
        myAddr = rad;
        deviceAllocated = true;
        cdState = 0xC0;
        initWaitTime = 0;
        // ATRYBUTY ZGODNE Z PRAWDZIWA ZMIENIARKA (CDX-M670 sniff, adres 0x31):
        //   10 31 8C D0 | 9D | 04 A8 1F A3 | 0B 00
        // Bajty 5/6 (0x9D/0x04) skaluja sie z adresem tak, by ich suma byla
        // stala (0xA1): idx5 = 0x6C + addr, idx6 = 0xA1 - idx5. Dla 0x31 daje
        // dokladnie 9D/04 jak w sniffie; dla 0x32+ ekstrapolujemy ten wzor.
        uint8_t b5 = (uint8_t)(0x6C + myAddr);
        uint8_t b6 = (uint8_t)(0xA1 - b5);
        const uint8_t status[] = {0x10, myAddr, 0x8C, 0xD0, b5, b6, 0xA8, 0x1F, 0xA3, 0x0B, 0x00};
        sendRawPacket(status, sizeof(status));
        Serial.printf(">> Adres przydzielony: 0x%02X (op2=0x%02X)! deviceAllocated=true\n", myAddr, op2);
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
    else if (rad == myAddr && tad == 0x10 && op1 == 0x01 && op2 == 0x12) {
        if (cdState == 0xC0 && initWaitTime == 0) {
            initWaitTime = millis();
        }
        
        // Prawdziwa zmieniarka odpowiada: 10 <addr> 00 <state> <parity> 00
        // np. 10 31 00 C0 01 00 (init), 10 31 00 80 C1 00 (idle), 10 31 00 00 41 00 (play)
        sendShortResponse(0x10, myAddr, 0x00, cdState);
        
        // Debug
        Serial.printf(">> PING odpowiedź: stan=0x%02X\n", cdState);
    }
    
    // ===== 5. UPDATE DISPLAY (31 10 01 13) =====
    // TYLKO gdy tad == 0x10 (pytanie skierowane do nas przez main controller)
    // NIE odpowiadamy na 31 14 01 13 - to jest pytanie do display processora (0x14), nie do nas!
    else if (rad == myAddr && tad == 0x10 && op1 == 0x01 && op2 == 0x13) {
        needDisplayUpdate = false; // Obsłużone

        // PAKIET ZGODNY Z PRAWDZIWA ZMIENIARKA (CDX-M670 sniff).
        // Prawdziwa zmieniarka NIE wysyla wymyslonego "70 31 90 00" — wysyla
        // autentyczny status CD jako 16-bajtowy pakiet:
        //   70 31 C0 00 | P1 | 00 00 00 00 30 <TRK> <MIN> <SEK> 88 | P2 | 00
        // gdzie (sniff: ...30 F1 F0 00 88 = TR1 0:00, ...30 F3 F0 00 88 = TR3):
        //   D5  = 0x30  (marker stalej)
        //   D6  = TRACK BCD (F-padding: F1=01..F9=09, potem 10..99)
        //   D7  = MINUTY BCD (F-padding: F0=0)
        //   D8  = SEKUNDY BCD (00..59)
        //   D9  = NUMER PLYTY w starszym nibblu (sniff: 0x10=CD1, 0x80=CD8,
        //         0x90=CD9). Wczesniej bylo na sztywno 0x88 -> wyswietlacz
        //         pokazywal ciagle "disc 8". Teraz dynamicznie: (disc << 4).
        // cmd2 trzymamy 0x00 (Playing) zawsze — stan load/seek radio bierze z
        // PING-a (01 12), a tu unikamy migania "LOAD" przy szybkich zmianach.
        // Ten autentyczny status jest tez najlepszym kandydatem na sygnal,
        // ktory sklania radio do otwarcia wejscia "BUS AUDIO IN".
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

        uint8_t discByte = (uint8_t)((currentDisk & 0x0F) << 4); // CD1->0x10 ... CD10->0xA0

        sendLongResponse(0x70, myAddr, 0xC0, 0x00,
                         0x00, 0x00, 0x00, 0x00,
                         0x30, trackBCD, minBCD, secBCD, discByte);

        Serial.printf(">> DISPLAY (C0): CD%d TR%d %02d:%02d (D9=0x%02X, state=0x%02X)\n",
                     currentDisk, currentTrack, playMinutes, playSeconds, discByte, cdState);
    }
    
    // ===== 6. WAKE UP / PLAY command (3X 10 20 00) =====
    else if (rad == myAddr && tad == 0x10 && op1 == 0x20 && op2 == 0x00) {
        // Radio potrafi WIELOKROTNIE wysylac PLAY w trakcie grania (widoczne w
        // logach co kilka sekund). Jesli juz gramy, NIE restartuj utworu od zera.
        // Dotyczy to TAKZE sytuacji po SYSTEM RESET radia w trakcie grania:
        // audio nie zostalo zatrzymane (gra dalej przez cala re-inicjalizacje),
        // wiec zamiast przeladowac plik od 00:00 wracamy tylko do stanu Playing
        // i synchronizujemy licznik czasu z realna pozycja w utworze.
        if (audioIsPlaying()) {
            if (cdState != 0x00) {
                uint32_t t = audioGetCurrentTimeSec();
                playMinutes = t / 60;
                playSeconds = t % 60;
                lastSecondTick = millis();
                cdState = 0x00;
                needDisplayUpdate = true;
                Serial.printf(">> PLAY (wznowienie bez restartu, np. po resecie radia: %02d:%02d)\n",
                              playMinutes, playSeconds);
            } else {
                Serial.println(">> PLAY (juz gram — ignoruje, nie restartuje utworu)");
            }
        } else {
            Serial.println(">> PLAY command received!");
            enterSeekMode();
        }
    }
    
    // ===== 7. Button commands from front panel (3X 11 XX XX) =====
    else if (rad == myAddr && tad == 0x11) {
        if (op1 == 0x26 && op2 == 0x10) {
            // NEXT TRACK — z uwzględnieniem rzeczywistej liczby tracków
            uint8_t maxTr = audioGetTrackCount(currentDisk);
            if (maxTr == 0) maxTr = MAX_TRACK_PER_DISC; // fallback bez SD
            if (currentTrack < maxTr) {
                currentTrack++;
                enterSeekMode();
                Serial.printf(">> NEXT TRACK: CD%d TR%d (max=%d)\n", currentDisk, currentTrack, maxTr);
            } else {
                Serial.println(">> NEXT TRACK zablokowany: to ostatni utwór na płycie.");
            }
        } 
        else if (op1 == 0x27 && op2 == 0x10) {
            // PREV TRACK (z logiką restartu piosenki po 2s)
            uint32_t currentSec = playSeconds;
            if (audioIsPlaying()) currentSec = audioGetCurrentTimeSec();
            
            if (currentSec > 2) {
                // Cofnij na początek obecnego utworu
                enterSeekMode();
                Serial.printf(">> PREV TRACK (Restart utworu): CD%d TR%d\n", currentDisk, currentTrack);
            } else {
                // Cofnij do poprzedniego utworu
                if (currentTrack > 1) {
                    currentTrack--;
                    enterSeekMode();
                    Serial.printf(">> PREV TRACK: CD%d TR%d\n", currentDisk, currentTrack);
                } else {
                    // Jesteśmy na 1 utworze, <= 2s -> cofnij na początek pierwszego utworu
                    enterSeekMode();
                    Serial.println(">> PREV TRACK (Restart 1 utworu, brak poprzedniego)");
                }
            }
        }
        else if (op1 == 0x28) {
            // NEXT DISC — przeskocz puste dyski
            uint8_t nextDisc = audioFindNextNonEmptyDisc(currentDisk);
            if (nextDisc != 0) {
                currentDisk = nextDisc;
            } else {
                // Żaden dysk nie ma plików — klasyczne zachowanie
                currentDisk++;
                if (currentDisk > MAX_DISC) currentDisk = 1;
            }
            currentTrack = 1;
            enterSeekMode();
            Serial.printf(">> NEXT DISC: CD%d\n", currentDisk);
        }
        else if (op1 == 0x29) {
            // PREV DISC — przeskocz puste dyski
            uint8_t prevDisc = audioFindPrevNonEmptyDisc(currentDisk);
            if (prevDisc != 0) {
                currentDisk = prevDisc;
            } else {
                if (currentDisk <= 1) currentDisk = MAX_DISC;
                else currentDisk--;
            }
            currentTrack = 1;
            enterSeekMode();
            Serial.printf(">> PREV DISC: CD%d\n", currentDisk);
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
            // Magistrala wylaczona (radio uspione/wylaczone) — procedura SNU.
            // Prawdziwa zmieniarka jest zasilana z magistrali i po prostu gasnie.
            // My zatrzymujemy dzwiek i zapamietujemy ostatni utwor, zeby po
            // ponownym wlaczeniu wznowic od tego samego miejsca.
            Serial.println("=== SEN: magistrala wylaczona — zatrzymuje audio ===");
            persistLastTrack();
            audioStop();
            deviceAllocated = false;
            cdState = 0xC0;
            initWaitTime = 0;
            wantSlaveBreak = false;
            needDisplayUpdate = false;
            myAddr = 0x31;
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
        } else {
            // Pobudka — magistrala znow zasilona. Upewnij sie, ze DAC ma
            // ustawiona docelowa glosnosc (na wypadek gdyby cos ja zmienilo).
            audioSetVolume(AUDIO_VOLUME);
        }
    }

    // Dynamicznie dostosuj płyty przy pierwszym montowaniu USB
    static bool wasUsbDriveMounted = false;
    bool isUsbDriveMounted = audioGetTrackCount(currentDisk) > 0 || audioFindNextNonEmptyDisc(0) > 0;
    if (isUsbDriveMounted && !wasUsbDriveMounted) {
        wasUsbDriveMounted = true;
        // Jesli wczytana z NVS plyta ma utwory — zostajemy na niej (wznawiamy
        // ostatni utwor). W przeciwnym razie wybieramy pierwsza niepusta plyte.
        if (audioGetTrackCount(currentDisk) > 0) {
            // Zabezpiecz numer utworu do realnej liczby plikow na plycie
            uint8_t maxTr = audioGetTrackCount(currentDisk);
            if (currentTrack > maxTr) currentTrack = 1;
            Serial.printf(">>> Wykryto USB. Wznawiam zapamietana plyte: CD%d TR%d\n",
                          currentDisk, currentTrack);
        } else {
            uint8_t firstDisc = audioFindNextNonEmptyDisc(0);
            if (firstDisc != 0) {
                currentDisk = firstDisc;
                currentTrack = 1;
                Serial.printf(">>> Wykryto USB. Ustawiono pierwszą niepustą płytę: CD%d\n", currentDisk);
            }
        }
    } else if (!isUsbDriveMounted && wasUsbDriveMounted) {
        wasUsbDriveMounted = false;
    }

    // Timeout: radio zniknęło
    if (deviceAllocated && (now - lastPingTime > 5000)) {
        deviceAllocated = false;
        cdState = 0xC0;
        initWaitTime = 0;
        wantSlaveBreak = false;
        needDisplayUpdate = false;
        // Radio ucichlo (prawdopodobnie wylaczone) — zatrzymaj i zapamietaj.
        // To zapasowa procedura snu na wypadek gdyby pin BUS_ON nie spadl do 0.
        persistLastTrack();
        audioStop();
        myAddr = 0x31;
        Serial.println("--- Radio timeout (5s). Reset do C0 + STOP audio ---");
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

    // Sekundnik — własny, płynny pomiar czasu co równo 1000ms
    // Uniezależniamy się od czasu dekodera audio, który potrafił "przeskakiwać"
    // przy wczytywaniu bufora lub pokazywać stare wartości ułamek sekundy po starcie nowej piosenki.
    if (cdState == 0x00 && (now - lastSecondTick >= 1000)) {
        // Aby uniknąć poślizgów (dryftu), dodajemy równe 1000
        lastSecondTick += 1000;
        // Zabezpieczenie przed ewentualnym zacięciem pętli
        if (now - lastSecondTick > 1000) lastSecondTick = now;

        playSeconds++;
        if (playSeconds >= 60) {
            playSeconds = 0;
            playMinutes++;
            if (playMinutes >= 100) playMinutes = 0;
        }
        needDisplayUpdate = true;
    }
    
    // Auto-next: piosenka się skończyła (callback z AudioPlayer)
    if (audioSongFinished()) {
        uint8_t maxTr = audioGetTrackCount(currentDisk);
        if (maxTr == 0) maxTr = MAX_TRACK_PER_DISC;
        
        currentTrack++;
        if (currentTrack > maxTr) {
            currentTrack = 1;
            uint8_t nextDisc = audioFindNextNonEmptyDisc(currentDisk);
            if (nextDisc != 0) {
                currentDisk = nextDisc;
            } else {
                currentDisk++;
                if (currentDisk > MAX_DISC) currentDisk = 1;
            }
        }
        enterSeekMode();
        Serial.printf(">> AUTO-NEXT: CD%d TR%d\n", currentDisk, currentTrack);
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
    // SLAVE BREAK potrzebny: aktualizacja WYSWIETLACZA jest inicjowana przez
    // slave'a. Master sam NIGDY nie pyta o ekran — dopiero gdy wykryje nasz
    // break, wysyla 01 15 ("kto chce ekran?") i 01 13 ("dawaj dane"). Bez tego
    // ekran zostaje pusty (potwierdzone w logach). Robimy to bezpiecznie:
    //  - tylko po >8ms ciszy (czyli po ACK, w realnym idle miedzy transakcjami),
    //  - z natychmiastowym PORZUCENIEM gdy radio ruszy z zegarem (issueSlaveBreak),
    //  - a przekłamane ramki i tak odrzucamy po sumie kontrolnej.
    const bool ENABLE_SLAVE_BREAK = true;
    if (ENABLE_SLAVE_BREAK &&
        needDisplayUpdate && silToBreak && !busy && deviceAllocated && stateAllowsBreak &&
        busPowered && millis() >= suppressBreakUntil) {
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
    
    // Obsługa dekodowania audio — MUSI być wywoływane w każdej iteracji!
    audioLoop();
}
