#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// =============================================================================
// Config.h — centralna konfiguracja emulatora zmieniarki Sony UniLink
// =============================================================================
// Wszystkie piny, ustawienia sprzetowe i stale czasowe trzymamy w jednym
// miejscu, zeby strojenie protokolu nie wymagalo szukania po wielu plikach.
//
// UWAGA: wartosci czasowe ponizej sa wynikiem mozolnego strojenia pod radia
// CDX-M670 / MEX-BT3800u. Zmiana ktorejkolwiek z nich moze wywolac petle
// SYSTEM RESET. Szczegoly w komentarzach przy poszczegolnych stalych.
// =============================================================================

// --- KONFIGURACJA PINOW MAGISTRALI UNILINK ---
constexpr uint8_t PIN_BUS_ON = 4;   // zasilanie magistrali (HIGH = radio wlaczone)
constexpr uint8_t PIN_CLOCK  = 5;   // zegar magistrali (przerwanie)
constexpr uint8_t PIN_DATA   = 6;   // linia danych (dwukierunkowa)

// --- USTAWIENIA SPRZETOWE WARSTWY FIZYCZNEJ ---
constexpr bool INVERT_DATA = true;     // sprzetowy inwerter na linii DATA
constexpr int  CLOCK_EDGE  = RISING;   // zbocze zegara, na ktorym probkujemy bit

// --- ROZMIARY BUFOROW ---
constexpr int RX_BUFFER_SIZE = 64;
constexpr int TX_BUFFER_SIZE = 64;

// --- LIMITY WIRTUALNEJ ZMIENIARKI (fallback gdy nosnik nie zamontowany) ---
constexpr uint8_t MAX_TRACK_PER_DISC = 99;
constexpr uint8_t MAX_DISC           = 10;

// --- ADRESY UNILINK ---
constexpr uint8_t ADDR_DEFAULT   = 0x31;   // domyslny adres zmieniarki (do re-adopcji)
constexpr uint8_t ADDR_BROADCAST = 0x18;   // adres rozglosza (ANYONE? itp.)
constexpr uint8_t ADDR_MASTER    = 0x10;   // TAD radia (ramki OD mastera)
constexpr uint8_t ADDR_DISPLAY   = 0x14;   // TAD procesora ekranu radia

// --- STROJENIE CZASOW MASZYNY STANOW (ms) ---
constexpr unsigned long INIT_DURATION_MS = 800;  // 0xC0 -> 0x80 (po pierwszym PINGu)
constexpr unsigned long LOAD_DURATION_MS = 50;   // 0x40 -> 0x20 (krotki, by uniknac migania LOAD)
constexpr unsigned long SEEK_DURATION_MS = 50;   // 0x20 -> 0x00 (krotki, by uniknac migania LOAD)

// --- STROJENIE SLAVE BREAK ---
constexpr unsigned long BREAK_INTERVAL_MS = 150;   // minimalny odstep miedzy Slave Breakami
constexpr unsigned long READ_SILENCE_US   = 5000;  // cisza po ktorej przetwarzamy odebrana ramke
// BREAK_SILENCE_US: wymagana CISZA przed Break. MUSI byc TUZ POWYZEJ przerwy
// command->ack radia (~6ms w sniffie). Przy 6000us lapalismy moment nadejscia
// ACK -> kolizja -> SYSTEM RESET. Przy 8000us ACK juz przyszedl. Za duzo (12000)
// = break sie nie wyzwala (czarny ekran).
constexpr unsigned long BREAK_SILENCE_US  = 8000;
// BREAK_HOLD_US: jak dlugo trzymac DATA LOW (na tyle, by radio wykrylo Break).
constexpr unsigned long BREAK_HOLD_US     = 2500;

// --- OCHRONA PRZED KOLIZJA Z URZADZENIAMI WEWNETRZNYMI RADIA ---
// Radio odpytuje swoje urzadzenia (0x3B = CD radia, 0x71 = kontroler), ktore
// odpowiadaja z opoznieniem ~9-12ms. Gdy zobaczymy poll do INNEGO urzadzenia,
// blokujemy break na to okno, az tamto zdazy odpowiedziec.
constexpr unsigned long FOREIGN_POLL_GUARD_MS = 30;

// --- DETEKCJA CDX-M670 ---
// Po markerze preliminary (3B/DB) ignorujemy ANYONE? przez to okno, by radio
// dokonczylo preliminary discovery bez nas (tak robi prawdziwa zmieniarka).
constexpr unsigned long PRELIMINARY_WINDOW_MS = 250;

// --- TIMEOUTY ---
constexpr unsigned long RADIO_TIMEOUT_MS = 5000;   // brak PINGa => radio zniknelo

// Jak dlugo po ostatnim zadaniu ekranu (01 13) uznajemy, ze radio AKTYWNIE
// pollu­je nasz wyswietlacz. W tym czasie NIE wystawiamy Slave Break — bylby
// zbedny (radio i tak co chwile pyta o ekran), a grozilby kolizja z wewnetrznym
// CD radia (0x3B) / procesorem ekranu (0x14) => utrata sync => SYSTEM RESET.
constexpr unsigned long DISPLAY_POLL_ACTIVE_MS = 1000;

// --- PAMIEC NIEULOTNA (NVS) ---
constexpr const char* PREFS_NAMESPACE = "unilink";

// --- LOGOWANIE ---
// Logowanie szczegolowe: zrzut KAZDEJ ramki RX oraz rutynowe odpowiedzi
// (Slave Poll 01 15, PING 01 12, DISPLAY 01 13, diagnostyka audio).
// DOMYSLNIE WYLACZONE: w trakcie odtwarzania radio odpytuje wyswietlacz ~10x/s,
// co przy wlaczonym logowaniu zalewa konsole i — co wazniejsze — intensywne
// Serial.printf w petli glownej moze ja blokowac na tyle, by spowodowac
// kolizje na magistrali i SYSTEM RESET radia. Wlacz tylko do debugowania
// discovery/przydzialu adresu. Wazne, rzadkie zdarzenia loguja sie zawsze.
constexpr bool DEBUG_VERBOSE = false;

#endif // CONFIG_H
