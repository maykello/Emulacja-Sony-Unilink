#include <Arduino.h>

// =====================================================================
//  Sony UniLink SPY  v2  (PASYWNY SNIFFER ESP32-S3)
//  Cel: zarejestrowac CALA komunikacje na magistrali tak wiernie jak
//  to mozliwe, z pelnym wykorzystaniem BUS_ON + CLOCK + DATA.
//
//  Co nowego wzgledem v1:
//   - BUS_ON obslugiwany przerwaniem: log zmian stanu + RESYNC ramki
//     przy (re)aktywacji magistrali (czyste granice sesji).
//   - Bufor pierscieniowy + streaming w loop() => nie gubimy bajtow
//     przy dlugich seriach (np. discovery), flaga przepelnienia.
//   - Resynchronizacja ramkowania bajtow po granicy SLOWA (duza przerwa)
//     oraz opcjonalnie po przerwie miedzy-bajtowej => pojedynczy glitch
//     nie rozjezdza calej reszty transmisji.
//   - Czysty, BEZSTRATNY format: jedna RAMKA (slowo) = jedna linia,
//     z timestampem (us), przerwa przed ramka (dt) i znacznikiem '!'
//     dla bajtow zlozonych z liczby bitow != 8 (podejrzane).
//
//  UWAGA: pasywny podsluch. Nie zapomnij o konwerterach napiec 5V->3V3!
// =====================================================================

// --- KONFIGURACJA PINOW ---
#define PIN_BUS_ON 4
#define PIN_CLOCK  5
#define PIN_DATA   6

// --- USTAWIENIA SPRZETOWE (jak w dzialajacym emulatorze) ---
const bool INVERT_DATA = true;
const int  CLOCK_EDGE  = RISING;

// --- PROGI CZASOWE ---
// Bajty w obrebie jednego slowa ida szybko obok siebie (~1 ms miedzy bajtami),
// a slowa (ramki) dzieli duza przerwa (~6 ms). Prog 3 ms czysto je rozdziela.
const unsigned long WORD_GAP_US = 3000;
// Resync ramkowania po przerwie miedzy-bajtowej. 0 = wylaczone (resync tylko
// na granicy slowa, bezpieczne). Ustaw np. 500 DOPIERO po zmierzeniu realnego
// rozstawu bitow (patrz auto-pomiar "minBitGap" wypisywany przy starcie sesji).
const unsigned long BYTE_GAP_US = 0;

// =====================================================================
//  BUFOR PIERSCIENIOWY (producent: ISR, konsument: loop)
// =====================================================================
#define RB_SIZE 2048
volatile uint8_t  rbByte[RB_SIZE];
volatile uint32_t rbGap[RB_SIZE];   // przerwa od poprzedniego bajtu [us]
volatile uint32_t rbTime[RB_SIZE];  // znacznik czasu (micros) zamkniecia bajtu
volatile uint8_t  rbBits[RB_SIZE];  // ile bitow mial bajt (norma 8)
volatile uint16_t rbHead = 0;
volatile uint16_t rbTail = 0;
volatile bool     rbOverflow = false;

// --- Stan dekodera bitow ---
volatile uint8_t  incomingByte = 0;
volatile int      bitIndex = 0;
volatile unsigned long lastEdge  = 0;  // czas ostatniego zbocza zegara
volatile unsigned long lastFinal = 0;  // czas zamkniecia ostatniego bajtu

// --- BUS_ON ---
volatile bool busEventPending = false;
volatile bool busState = false;

// --- Auto-pomiar najkrotszej przerwy miedzy zboczami (rozstaw bitow) ---
volatile uint32_t minEdgeGap = 0xFFFFFFFF;

bool readDataPin() {
  bool val = digitalRead(PIN_DATA);
  return INVERT_DATA ? !val : val;
}

// Makro zamykajace biezacy bajt i wrzucajace go do bufora pierscieniowego.
// Celowo makro (nie osobna funkcja IRAM) - osobna mala funkcja IRAM_ATTR
// wywolywana z ISR powoduje na Xtensa blad linkera
// "dangerous relocation: l32r: literal placed after use".
#define FINALIZE_BYTE(now, bits)                                   \
  do {                                                             \
    uint16_t _next = (uint16_t)((rbHead + 1) % RB_SIZE);           \
    if (_next == rbTail) {                                         \
      rbOverflow = true;                                           \
    } else {                                                       \
      rbByte[rbHead] = incomingByte;                               \
      rbGap[rbHead]  = (uint32_t)((now) - lastFinal);              \
      rbTime[rbHead] = (uint32_t)(now);                            \
      rbBits[rbHead] = (uint8_t)(bits);                            \
      rbHead = _next;                                              \
    }                                                              \
    lastFinal = (now);                                             \
    bitIndex = 0;                                                  \
    incomingByte = 0;                                              \
  } while (0)

// --- PRZERWANIE ZEGARA ---
void IRAM_ATTR onClockEvent() {
  unsigned long now = micros();
  unsigned long edgeGap = now - lastEdge;
  lastEdge = now;

  if (edgeGap < minEdgeGap) minEdgeGap = edgeGap;

  // Resync ramki: jesli mamy niedokonczony bajt, a wlasnie wystapila przerwa
  // wskazujaca granice (slowa lub bajtu), zamykamy go jako podejrzany.
  unsigned long resyncGap = (BYTE_GAP_US > 0) ? BYTE_GAP_US : WORD_GAP_US;
  if (bitIndex > 0 && edgeGap > resyncGap) {
    FINALIZE_BYTE(now, bitIndex); // bits != 8 => oznaczone '!'
  }

  if (readDataPin()) {
    incomingByte |= (1 << (7 - bitIndex));
  } else {
    incomingByte &= ~(1 << (7 - bitIndex));
  }
  bitIndex++;

  if (bitIndex >= 8) {
    FINALIZE_BYTE(now, 8);
  }
}

// --- PRZERWANIE BUS_ON ---
void IRAM_ATTR onBusEvent() {
  busState = digitalRead(PIN_BUS_ON);
  busEventPending = true;
  // (re)aktywacja/zmiana magistrali => czysty restart ramkowania
  bitIndex = 0;
  incomingByte = 0;
  lastEdge = micros();
  lastFinal = lastEdge;
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println("--- Sony UniLink SPY v2 (PASSIVE SNIFFER ESP32-S3) ---");
  Serial.println("Podlacz ESP rownolegle do magistrali. Konwertery napiec 5V->3V3!");
  Serial.println("Format: t=<us> dt=<przerwa_us> bus=<0/1> : BAJTY  ('!'=bajt != 8 bitow)");
  Serial.println("Zdarzenia: 'BUS=1/0' zmiana zasilania magistrali, 'minBitGap=...us' auto-pomiar.");
  Serial.println();

  pinMode(PIN_BUS_ON, INPUT);
  pinMode(PIN_CLOCK, INPUT);
  pinMode(PIN_DATA, INPUT);

  unsigned long t0 = micros();
  lastEdge  = t0;
  lastFinal = t0;
  busState  = digitalRead(PIN_BUS_ON);

  attachInterrupt(digitalPinToInterrupt(PIN_CLOCK), onClockEvent, CLOCK_EDGE);
  attachInterrupt(digitalPinToInterrupt(PIN_BUS_ON), onBusEvent, CHANGE);

  Serial.print("Start. BUS_ON=");
  Serial.println(busState ? 1 : 0);
}

// --- Skladanie bajtow w SLOWA (ramki) i druk ---
static uint8_t  wordBytes[64];
static uint8_t  wordBits[64];
static int      wordLen  = 0;
static uint32_t wordGap  = 0;   // przerwa przed pierwszym bajtem slowa
static uint32_t wordTime = 0;   // czas pierwszego bajtu slowa
static bool     wordBus  = false;

void flushWord() {
  if (wordLen == 0) return;
  Serial.print("t=");
  Serial.print(wordTime);
  Serial.print(" dt=");
  Serial.print(wordGap);
  Serial.print(" bus=");
  Serial.print(wordBus ? 1 : 0);
  Serial.print(" : ");
  for (int i = 0; i < wordLen; i++) {
    if (wordBytes[i] < 0x10) Serial.print("0");
    Serial.print(wordBytes[i], HEX);
    if (wordBits[i] != 8) Serial.print("!"); // bajt podejrzany (resync)
    Serial.print(" ");
  }
  Serial.println();
  wordLen = 0;
}

void loop() {
  // 1) Zdarzenia BUS_ON
  if (busEventPending) {
    noInterrupts();
    busEventPending = false;
    bool s = busState;
    interrupts();
    flushWord();
    Serial.print("BUS=");
    Serial.println(s ? 1 : 0);
  }

  // 2) Przepelnienie bufora
  if (rbOverflow) {
    noInterrupts();
    rbOverflow = false;
    interrupts();
    flushWord();
    Serial.println("!! BUFFER OVERFLOW - utracono bajty (zwieksz RB_SIZE / baud) !!");
  }

  // 3) Drenaz bufora pierscieniowego -> skladanie slow
  while (true) {
    noInterrupts();
    bool empty = (rbTail == rbHead);
    interrupts();
    if (empty) break;

    noInterrupts();
    uint8_t  b = rbByte[rbTail];
    uint32_t g = rbGap[rbTail];
    uint32_t t = rbTime[rbTail];
    uint8_t  bits = rbBits[rbTail];
    rbTail = (uint16_t)((rbTail + 1) % RB_SIZE);
    interrupts();

    // granica slowa?
    if (wordLen > 0 && g >= WORD_GAP_US) flushWord();

    if (wordLen == 0) {
      wordGap  = g;
      wordTime = t;
      wordBus  = digitalRead(PIN_BUS_ON);
    }
    if (wordLen < (int)sizeof(wordBytes)) {
      wordBytes[wordLen] = b;
      wordBits[wordLen]  = bits;
      wordLen++;
    } else {
      // slowo dluzsze niz bufor linii - wypisz i zacznij nowe
      flushWord();
      wordGap = g; wordTime = t; wordBus = digitalRead(PIN_BUS_ON);
      wordBytes[0] = b; wordBits[0] = bits; wordLen = 1;
    }
  }

  // 4) Flush slowa gdy magistrala ucichla
  noInterrupts();
  unsigned long le = lastEdge;
  uint32_t mbg = minEdgeGap;
  interrupts();
  if (wordLen > 0 && (micros() - le) > WORD_GAP_US) {
    flushWord();
    // przy okazji ciszy: raz na jakis czas pokaz zmierzony rozstaw bitow
    static uint32_t lastReported = 0;
    if (mbg != 0xFFFFFFFF && mbg != lastReported) {
      lastReported = mbg;
      Serial.print("minBitGap=");
      Serial.print(mbg);
      Serial.println("us");
    }
  }
}
