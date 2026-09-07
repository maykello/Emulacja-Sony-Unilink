// =============================================================================
// Emulator.ino — emulator zmieniarki CD Sony UniLink (10-CD) + audio z USB
// =============================================================================
// Obsluga radia: MEX-BT3800u + CDX-M670. Wyjscie audio: PCM5102A (I2S).
// Nosnik: pendrive USB (foldery CD01..CD10).
//
// Architektura (warstwy):
//   Config.h          — piny, ustawienia sprzetowe, stale czasowe
//   UnilinkBus        — warstwa fizyczna: bit-banging ISR, ramki, Slave Break
//   CdChanger         — model zmieniarki: stan, czas, NVS, nawigacja, audio
//   UnilinkProtocol   — interpretacja ramek, discovery, kwirki CDX-M670
//   AudioPlayer       — dekodowanie i odtwarzanie plikow (I2S/PCM5102A)
//   UsbDrive          — USB Host MSC (FAT32)
//   Emulator.ino      — spiecie warstw (setup + loop)
// =============================================================================

#include "AudioPlayer.h"
#include "CdChanger.h"
#include "Config.h"
#include "UnilinkBus.h"
#include "UnilinkProtocol.h"
#include <Arduino.h>
#include <LittleFS.h>

// Stan zasilania magistrali (do wykrywania zboczy BUS_ON).
static bool busPoweredLast = false;
static bool powerLatchActive = false;
static unsigned long busOffStartTime = 0;

void commitSuicide() {
  if (powerLatchActive) {
    Serial.println("=== ROZPOCZYNAM PROCEDURĘ WYŁĄCZANIA ZASILANIA ===");
    Serial.flush();
    delay(50); // daj czas klientowi TCP na odebranie pakietu

    // Zapisz czarną skrzynkę na USB (jeśli zamontowane)
    Serial.dumpBlackbox();

    // Czekaj chwile, zeby logi dotarly przez UART i NVS sie zapisal
    delay(100);

    // Odcięcie zasilania
    Serial.println("=== ODCINAM PRĄD. DOBRANOC. ===");
    Serial.flush();

    // UWAGA: Serial.flush() dla WiFiClient upewnia się tylko, że dane weszły
    // do stosu TCP (LwIP). Musimy dać modułowi radiowemu ESP32 dodatkowy czas
    // na fizyczne wyemitowanie tych pakietów w eter zanim zgasimy zasilanie.
    // Bez tego delay'a układ odetnie zasilanie mikrosekundy po zapisaniu do
    // pamięci, a komputer nigdy nie zobaczy ostatnich linijek loga.
    delay(5000);

    digitalWrite(PIN_POWER_LATCH, LOW);
    powerLatchActive = false;

    // W tym miejscu w realnym ukladzie ESP32 zgasnie (odciecie zasilania).
    // Jesli plytka jest podpieta pod USB z PC lub kondensatory jeszcze trzymaja napiecie,
    // czekamy pasywnie na calkowity zanik zasilania lub na powrot BUS_ON = HIGH.
    while (digitalRead(PIN_BUS_ON) == LOW) {
      delay(50);
    }
    busOffStartTime = 0;
  }
}

void setup() {
  // --- NATYCHMIASTOWE PRZEJECIE ZASILANIA (Suicide Circuit) ---
  pinMode(PIN_POWER_LATCH, OUTPUT);
  digitalWrite(PIN_POWER_LATCH, HIGH);
  powerLatchActive = true;

  Serial.begin(921600); // wyzszy baud: pelne logowanie ramek (DEBUG_FRAMES)
                        // nie obciaza petli (przy 115200 ~12% czasu = ryzyko
                        // kolizji). Logger musi uzywac tego samego baudu.
  Serial.println("--- Sony UniLink EMULATOR (10-CD) v9 + AUDIO ---");
  Serial.println("Obsluga: MEX-BT3800u + CDX-M670 + PCM5102A DAC + USB");
  Serial.println("Oczekuje na radio (Stan C0 - Init)...");

  // Inicjalizacja LittleFS dla pamieci podrecznej (indeks pendrive'a)
  if (!LittleFS.begin(true)) {
    Serial.println("[LittleFS] UWAGA: Błąd inicjalizacji partycji LittleFS!");
  }

  // Pamiec nieulotna — wczytaj ostatnio odtwarzany utwor.
  CdChanger::begin();

  // --- MAGISTRALA STARTUJE JAKO PIERWSZA ---
  // Musimy reagowac na 'Ping' radia od pierwszych milisekund.
  UnilinkBus::begin();
  UnilinkProtocol::begin();

  // --- AUDIO I USB ---
  // Startuje nieblokujaco w tle.
  if (audioInit()) {
    Serial.println("[Audio] Inicjalizacja zlecona (dziala w tle).");
  } else {
    Serial.println("[Audio] UWAGA: Inicjalizacja I2S nie powiodła się.");
  }
}

// ===== ODBIOR I PRZETWARZANIE RAMEK =====
// Master wymaga odpowiedzi w scisle okreslonym oknie (w sniffie kolejne ramki
// dziela ~6 ms, a odpowiedz slave'a pada w nastepnym slocie). Dlatego:
//   * tniemy strumien po DLUGOSCI Z CMD1 (UnilinkBus::readFrame), a nie po
//     ciszy — ramka jest gotowa do obsluzenia w chwili odebrania ostatniego
//     bajtu, a nie 5 ms pozniej,
//   * oprozniamy CALY bufor w jednej iteracji, bo w jednym przebiegu petli
//     potrafi sie zebrac kilka ramek (poll -> grant -> komenda),
//   * wolamy to KILKA RAZY w petli, przeplatajac z reszta obowiazkow.
// Poprzednia wersja uzywala readPacketIfIdle(5 ms) i brala tylko jedna porcje
// na iteracje: krotkie ramki sklejaly sie z bajtem wypelniacza, dlugie byly
// ciete w polowie, a odpowiedzi spozialy sie na tyle, ze radio resetowalo
// magistrale.
static void pumpBus(bool busPowered) {
  static uint8_t packet[RX_BUFFER_SIZE];
  for (int guard = 0; guard < 8; ++guard) {
    if (UnilinkBus::isTransmitting())
      return;
    int count = UnilinkBus::readFrame(packet, sizeof(packet));
    if (count <= 0)
      return;
    if (!busPowered)
      continue; // przy BUS=0 pochlaniamy bez odpowiedzi

    if (DEBUG_FRAMES) {
      static unsigned long lastRxMs = 0;
      unsigned long nowRx = millis();
      Serial.printf("[+%4lums] RX ", lastRxMs ? (nowRx - lastRxMs) : 0);
      lastRxMs = nowRx;
      for (int i = 0; i < count; i++) {
        if (packet[i] < 0x10)
          Serial.print("0");
        Serial.print(packet[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
    }

    UnilinkProtocol::handlePacket(packet, count);
  }
}

void loop() {
  // Magistrala ma bezwzgledne pierwszenstwo przed cala reszta obowiazkow.
  bool busPoweredNow = (digitalRead(PIN_BUS_ON) == HIGH);
  pumpBus(busPoweredNow);

  // ===== Detekcja zasilania magistrali (BUS_ON) =====
  // Prawdziwa zmieniarka jest zasilana z magistrali: gdy BUS=0, jest WYLACZONA
  // i nie odpowiada. CDX-M670 wykorzystuje fazy BUS=0/1 do dyskryminacji
  // urzadzen wewnetrznych od zewnetrznych — odpowiedz przy BUS=0 = petla RESET.
  if (busPoweredNow) {
    // BUS_ON aktywny — upewnij sie, ze pin podtrzymania zasilania jest HIGH
    if (!powerLatchActive) {
      digitalWrite(PIN_POWER_LATCH, HIGH);
      powerLatchActive = true;
      Serial.println("=== BUS_ON aktywny: Podtrzymanie zasilania aktywne (HIGH) ===");
    }
    // Kazde pojawienie sie HIGH natychmiast anuluje odliczanie do wylaczenia zasilania
    busOffStartTime = 0;

    if (!busPoweredLast) {
      busPoweredLast = true;
      Serial.println("=== BUS_ON = 1 ===");
      CdChanger::wake();
    }
  } else {
    // BUS_ON nieaktywny (LOW)
    if (busPoweredLast) {
      busPoweredLast = false;
      Serial.println("=== BUS_ON = 0 ===");
      Serial.println("=== SEN: magistrala wylaczona ===");
      CdChanger::sleep();
      UnilinkProtocol::onBusOff();
      UnilinkBus::resetRx(); // bajty z fazy BUS=0 sa "obce"
    }

    // Odliczanie do procedury samobojczej (commitSuicide po ciaglych 4s braku BUS_ON)
    if (busOffStartTime == 0) {
      busOffStartTime = millis() ? millis() : 1;
      Serial.printf("=== BUS_ON = 0: Oczekiwanie %lums na wylaczenie zasilania ===\n",
                    BUS_OFF_SUICIDE_DELAY_MS);
    } else if (millis() - busOffStartTime >= BUS_OFF_SUICIDE_DELAY_MS) {
      Serial.println("=== BUS_ON nieaktywny przez 4s: Wylaczam zasilanie (commitSuicide) ===");
      commitSuicide();
      busOffStartTime = 0; // nie powtarzaj procedury jesli plytka jest nadal zasilana (np. z USB)
    }
  }

  bool busPowered = busPoweredNow;

  pumpBus(busPowered);

  // ===== Wykrycie nosnika USB (wznowienie zapamietanej plyty) =====
  CdChanger::serviceMediaMount();

  // ZNACZNIK CZASU BIERZEMY DOPIERO TERAZ — PO OBSLUDZE MAGISTRALI.
  // handlePacket() zapisuje wlasne znaczniki przez millis() (lastPingTime w
  // UnilinkProtocol, initWaitTime/seekStartTime w CdChanger). Gdyby `now`
  // pochodzilo z POCZATKU petli, byloby STARSZE od tych znacznikow, a roznica
  // `now - znacznik` na typie bez znaku podwinelaby sie do ~4 mld ms. Kazdy
  // warunek "minelo juz X ms" spelnialby sie wtedy NATYCHMIAST: timeout radia
  // kasowal przydzielony adres w 3 ms po jego otrzymaniu (radio pytalo potem
  // `31 10 01 12`, my milczelismy jako 0x30 i radio szlo w SYSTEM RESET), a
  // maszyna mechanizmu przeskakiwala stany bez czekania.
  unsigned long now = millis();

  // ===== Timeout: radio zniknelo =====
  if (UnilinkProtocol::serviceTimeout(now)) {
    // Radio przestało z nami gadać
    // ... (już zalogowane wewnątrz UnilinkProtocol)
    // Zrzucamy czarną skrzynkę na pendrive, bo to prawdopodobnie crash lub błąd
    // komunikacji!
    Serial.dumpBlackbox();
    CdChanger::sleep();
    Serial.println("--- Radio timeout (5s). Reset do C0 + STOP audio ---");
  }

  // ===== Maszyna stanow + sekundnik =====
  CdChanger::update(now, UnilinkProtocol::isAllocated());

  // ===== Harmonogram ramki pozycji 0x90 (1Hz w stanie Playing) =====
  UnilinkProtocol::servicePositionFrame1Hz(now);

  // ===== Harmonogram ramki pelnego statusu 0xC0 (co ~30s w stanie Playing)
  // =====
  UnilinkProtocol::serviceFullStatusFrame(now);

  // ===== CD-TEXT wypychany po zmianie utworu/plyty (bez zadania radia) =====
  UnilinkProtocol::serviceCdText(now);

  // ===== Auto-next po koncu utworu =====
  CdChanger::serviceAutoAdvance();

  // ===== Auto-powtarzanie przewijania przy przytrzymanym FF/REW =====
  CdChanger::serviceSeekRepeat(now);

  // ===== Slave Break (OE: wakeup Request Polling na tik ~1 Hz) =====
  UnilinkProtocol::serviceSlaveBreak(busPowered);

  pumpBus(busPowered);

  // ===== Dekodowanie audio (hot-plug + wykrywanie konca utworu) =====
  audioLoop();

  pumpBus(busPowered);

  // ===== Odroczony zapis NVS — tylko gdy magistrala bezczynna =====
  CdChanger::servicePersist(UnilinkBus::microsSinceLastClock());
  UnilinkProtocol::servicePersist();

  // ===== Lekka diagnostyka (1 linia / 2s): czy radio nas pollu­je o ekran
  // =====
  UnilinkProtocol::serviceStats(now);

  // ===== Obsługa połączeń WiFi i klienta logów TCP =====
  WiFiLogger.loop();
}
