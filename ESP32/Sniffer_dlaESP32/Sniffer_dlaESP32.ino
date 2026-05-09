#include <Arduino.h>

// --- KONFIGURACJA PINÓW ---
#define PIN_BUS_ON 4
#define PIN_CLOCK 5
#define PIN_DATA 6

// --- USTAWIENIA SPRZĘTOWE ---
// Używamy tych samych ustawień co w V16, bo odbiór działał tam dobrze
const bool INVERT_DATA = true;
const int CLOCK_EDGE = RISING;

volatile uint8_t incomingByte = 0;
volatile int bitIndex = 0;

// Bufor na całą rozmowę
#define BUFFER_SIZE 256
volatile uint8_t packetBuffer[BUFFER_SIZE];
volatile unsigned long timeDeltas[BUFFER_SIZE]; // Czas od poprzedniego bajtu
volatile int packetIndex = 0;
volatile unsigned long lastByteTime = 0;
volatile unsigned long lastClockTime = 0;

// --- FUNKCJE SPRZĘTOWE ---

bool readDataPin() {
  bool val = digitalRead(PIN_DATA);
  return INVERT_DATA ? !val : val;
}

// --- PRZERWANIA ---

void IRAM_ATTR onClockEvent() {
  unsigned long now = micros();
  lastClockTime = now;

  if (readDataPin()) {
    incomingByte |= (1 << (7 - bitIndex));
  } else {
    incomingByte &= ~(1 << (7 - bitIndex));
  }

  bitIndex++;

  if (bitIndex > 7) {
    if (packetIndex < BUFFER_SIZE) {
      packetBuffer[packetIndex] = incomingByte;
      // Obliczamy czas jaki minął od zakończenia poprzedniego bajtu
      // To pozwoli nam zobaczyć przerwę między pytaniem Radia a odpowiedzią
      // Zmieniarki
      timeDeltas[packetIndex] = now - lastByteTime;
      lastByteTime = now;
      packetIndex++;
    }
    bitIndex = 0;
    incomingByte = 0;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("--- Sony UniLink SPY (PASSIVE SNIFFER ESP32) ---");
  Serial.println("Podlacz ESP rownolegle do dzialajacej zmieniarki.");
  Serial.println("Standard wyjscia dopasowany do logow UNO. Nie zapomnij o "
                 "konwerterach napiecia!");

  pinMode(PIN_BUS_ON, INPUT);
  pinMode(PIN_CLOCK, INPUT);
  pinMode(PIN_DATA, INPUT);

  // Inicjalizacja czasu
  lastByteTime = micros();

  attachInterrupt(digitalPinToInterrupt(PIN_CLOCK), onClockEvent, CLOCK_EDGE);
}

void loop() {
  // Sprawdzamy czy magistrala milczy (koniec transmisji pakietu)
  // Jeśli cisza trwa dłużej niż 10ms, wypisujemy to co zebraliśmy
  noInterrupts();
  bool silence = (micros() - lastClockTime > 10000);
  int count = packetIndex;
  interrupts();

  if (silence && count > 0) {
    // Kopiujemy dane żeby nie blokować przerwań na długo
    uint8_t tempBuf[BUFFER_SIZE];
    unsigned long tempTime[BUFFER_SIZE];

    noInterrupts();
    memcpy(tempBuf, (void *)packetBuffer, count);
    memcpy(tempTime, (void *)timeDeltas, count * sizeof(unsigned long));
    packetIndex = 0; // Reset bufora
    bitIndex = 0;
    interrupts();

    Serial.println("\n--- NOWA SEKWENCJA ---");

    for (int i = 0; i < count; i++) {
      // Logika wyświetlania:
      // Jeśli czas od poprzedniego bajtu jest duży (> 1000us), to znaczy że
      // była przerwa. W Unilink bajty w jednym słowie lecą szybko obok siebie.
      // Przerwa oznacza zmianę nadawcy (Radio -> Zmieniarka).

      if (tempTime[i] > 800) { // Przerwa większa niż 0.8ms oznacza "odpowiedź"
        Serial.println();      // Nowa linia dla czytelności
        Serial.print("   [GAP: ");
        Serial.print(tempTime[i]);
        Serial.print(" us] -> ");
      }

      if (tempBuf[i] < 0x10)
        Serial.print("0");
      Serial.print(tempBuf[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
  }
}