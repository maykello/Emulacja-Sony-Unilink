#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// =============================================================================
// Config.h — konfiguracja emulatora zmieniarki Sony Unilink
// =============================================================================
// Wartosci czasowe magistrali pochodza z pomiarow prawdziwej zmieniarki na
// CDX-M670 ("Python logger/unilink_log_zmieniarka_CDX-M670_*"):
//   * bit ~125 us, bajty bez przerw (~1 ms na bajt),
//   * ~5 ms ciszy zegara miedzy ramkami, po kazdej ramce slot odpowiedzi,
//   * ramka czasu 0x90 co ~1.0 s w trakcie odtwarzania,
//   * Break -> 18 10 01 15 po ~11-14 ms.
// =============================================================================

// --- PINY ---
constexpr uint8_t PIN_BUS_ON      = 4;   // zasilanie magistrali (HIGH = radio wlaczone)
constexpr uint8_t PIN_CLOCK       = 5;   // zegar magistrali (przerwanie)
constexpr uint8_t PIN_DATA        = 6;   // linia danych (dwukierunkowa)
constexpr uint8_t PIN_POWER_LATCH = 9;   // podtrzymanie przetwornicy (HIGH = wlaczone)

// --- WARSTWA FIZYCZNA ---
// Linia DATA jest aktywna stanem niskim: logiczna 1 = poziom dominujacy = LOW na
// pinie. Pusty slot odpowiedzi czyta sie jako 0x00.
constexpr bool INVERT_DATA = true;
constexpr int  CLOCK_EDGE  = RISING;   // zbocze, na ktorym probkujemy i wystawiamy bit

constexpr uint32_t BUS_CLOCK_GLITCH_US      = 30;    // zbocza blizej niz to = zaklocenie
constexpr uint32_t BUS_FRAME_GAP_US         = 1000;  // dluzsza przerwa = nowa ramka
// Odpowiedz musi byc uzbrojona zanim master zacznie taktowac slot (~5 ms po
// ostatnim bicie jego ramki). Pozniej juz NIE nadajemy — trafilibysmy w kolejna
// ramke mastera, co konczy sie resetem magistrali.
constexpr uint32_t BUS_RESPONSE_DEADLINE_US  = 4000;
constexpr uint32_t BUS_TX_NO_SLOT_TIMEOUT_US = 9000;  // master nie zaczal slotu
constexpr uint32_t BUS_TX_STALL_TIMEOUT_US   = 2000;  // master przestal taktowac

// --- SLAVE BREAK (fala bezczynnosci ~8 ms dominujaca / ~8 ms recesywna) ---
constexpr uint32_t BREAK_QUIET_BEFORE_US = 8000;   // po ostatnim zboczu (za slotem odpowiedzi)
constexpr uint32_t BREAK_SEARCH_MAX_US   = 40000;  // ile maks. czekamy na fale (2.5 okresu)
constexpr uint32_t BREAK_IDLE_LOW_MIN_US = 6000;   // min. dlugosc fazy dominujacej
constexpr uint32_t BREAK_IDLE_LOW_MAX_US = 30000;  // linia "przyklejona" — rezygnujemy
constexpr uint32_t BREAK_SETTLE_US       = 2000;   // opoznienie w fazie recesywnej
constexpr uint32_t BREAK_HOLD_US         = 3000;   // dlugosc impulsu

constexpr unsigned long BREAK_POLL_WAIT_MS    = 60;    // tyle czekamy na 18 10 01 15
constexpr unsigned long BREAK_RETRY_BUSY_MS   = 5;     // magistrala zajeta ramkami
constexpr unsigned long BREAK_RETRY_NOWAVE_MS = 30;    // brak fali bezczynnosci
constexpr unsigned long BREAK_RETRY_FAIL_MS   = 150;   // Break bez 01 15 (dalej x2)
constexpr unsigned long BREAK_BACKOFF_MAX_MS  = 2000;
constexpr unsigned long CLAIM_GRANT_WAIT_MS   = 60;    // zgloszenie 82 xx -> grant 01 13

// --- RAMKI EKRANU ---
// Czas: 0x90 przy kazdej zmianie sekundy (jak oryginal). Minimalny odstep
// pochlania tylko jitter; przy FF/REW czas zmienia sie szybciej niz 1 Hz.
constexpr unsigned long TIME_TICK_MIN_MS = 500;
constexpr unsigned long SEEK_TICK_MIN_MS = 250;

// --- SESJA Z RADIEM ---
constexpr uint8_t ADDR_GROUP_CD  = 0x30;   // grupa zmieniarek CD (adres przed przydzialem)
constexpr uint8_t ADDR_BROADCAST = 0x18;
constexpr uint8_t ADDR_MASTER    = 0x10;
constexpr uint8_t ADDR_DISPLAY   = 0x14;   // procesor ekranu radia (tez pinguje 01 12)

constexpr unsigned long BUS_ON_DEBOUNCE_MS     = 12;
// CDX-M670 robi discovery wewnetrznych urzadzen przy BUS_ON=0 przez ~220 ms.
// Dluzszy zanik = radio wylaczone: stop muzyki (adres zostaje).
constexpr unsigned long BUS_OFF_AUDIO_STOP_MS  = 500;
constexpr unsigned long BUS_OFF_SUICIDE_DELAY_MS = 4000;  // potem odciecie zasilania

constexpr unsigned long APPOINT_AFTER_ANYONE_MS = 300;   // appoint idzie ~10 ms po naszym 8C
constexpr unsigned long MAGIC_MIN_INTERVAL_MS   = 1500;  // 10 18 04 00 najwyzej raz na cykl
constexpr uint8_t       MAGIC_MAX_ATTEMPTS      = 6;
constexpr unsigned long MAGIC_BACKOFF_MS        = 30000;
constexpr unsigned long RADIO_SILENT_MS         = 5000;
// Ping pod nasz adres przychodzi co ~2.5 s. Brak przez tyle przy aktywnym radiu
// = radio zapomnialo o zmieniarce bez resetu magistrali.
constexpr unsigned long RADIO_FORGOT_US_MS      = 15000;

// --- ZAPIS FLASH (NVS/LittleFS) ---
// Operacja flash zatrzymuje zadania (przerwanie magistrali dziala dalej). Zapis
// robimy tuz po naszej odpowiedzi na ping — kolejny ping za ~2.5 s.
constexpr unsigned long FLASH_AFTER_APPOINT_MS         = 3000;
constexpr unsigned long FLASH_WINDOW_AFTER_PING_MIN_MS = 15;
constexpr unsigned long FLASH_WINDOW_AFTER_PING_MAX_MS = 400;

// --- ZADANIE MAGISTRALI ---
constexpr int      UNILINK_TASK_CORE     = 1;
constexpr unsigned UNILINK_TASK_PRIORITY = 20;
constexpr uint32_t UNILINK_TASK_STACK    = 8192;

// --- ZMIENIARKA ---
constexpr uint8_t MAX_TRACK_PER_DISC = 99;
constexpr uint8_t MAX_DISC           = 14;
constexpr const char* INDEX_FILE_PATH = "/unilink_index.dat";

constexpr unsigned long INIT_DURATION_MS = 800;  // 0xC0 -> 0x80 po pierwszym pingu
constexpr unsigned long LOAD_DURATION_MS = 80;   // 0x40 -> 0x20
constexpr unsigned long SEEK_DURATION_MS = 250;  // 0x20 -> 0x00; radio musi zobaczyc --:--

// --- PRZEWIJANIE FF/REW ---
// CDX-M670 wysyla 0x24/0x25 przy WCISNIECIU klawisza, a 18 10 08 00 przy
// puszczeniu. Pozycja plynie z przyspieszeniem zaleznym od czasu trzymania.
constexpr int           SEEK_STEP_SEC    = 1;      // kierunek (znak) dla CdChanger::seek
constexpr unsigned long SCAN_PHASE1_MS   = 1500;
constexpr unsigned long SCAN_PHASE2_MS   = 3000;
constexpr uint32_t      SCAN_RATE1       = 4;      // x4 materialu na sekunde
constexpr uint32_t      SCAN_RATE2       = 12;
constexpr uint32_t      SCAN_RATE3       = 30;
constexpr unsigned long SEEK_AUDIO_MS    = 1200;   // co ile skok dekodera (slyszalne cue)
constexpr unsigned long SEEK_SCAN_MAX_MS = 30000;  // gdyby 08 00 nie doszlo

// --- CD-TEXT ---
// CDX-M670 (wariant 0xD2): nazwa w dwoch ramkach po 6 znakow.
constexpr int           CDTEXT_D2_MAX_CHARS    = 12;
constexpr unsigned long OBD_UPDATE_INTERVAL_MS = 1000;   // tryb OBD (Repeat One/All)

// --- CRASH LOG (pendrive) ---
constexpr unsigned long CRASHLOG_GRACE_MS              = 60000;  // nie zapisuj startu radia
constexpr unsigned long CRASHLOG_BUS_ON_GRACE_MS       = 3000;   // 01 00 po BUS_ON = discovery
constexpr unsigned long CRASHLOG_DUMP_AFTER_BUS_OFF_MS = 600;
constexpr unsigned long USB_ERROR_GRACE_MS             = 8000;   // zanim "NO PENDRIVE"
constexpr int           CRASHLOG_MAX_FILES             = 10;

constexpr const char* PREFS_NAMESPACE = "unilink";

constexpr bool DEBUG_VERBOSE = false;

// Konfiguracja ENABLE_WIFI (0 = WiFi/BT wylaczone) znajduje sie w WiFiLogger.h.
#include "WiFiLogger.h"
#define Serial WiFiLogger

#endif // CONFIG_H
