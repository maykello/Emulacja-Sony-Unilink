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

// Krok przewijania (FF/REW) na pojedyncza komende 0x24/0x25 od radia.
constexpr int SEEK_STEP_SEC = 5;

// --- STROJENIE CZASOW (kompatybilne z protokolem §1) ---
// Okres bitu: ~20 µs (zgodnie z Kompendium §1). Zmiana TIMINGU BITU w przerwaniu
// onClockEvent (UnilinkBus.cpp) moze zaklocic komunikacje z radiem.
// Czas bajtu (8 bitów + przerwa): ~1 ms. Zmiana w czasie nadawania bajtu
// moze spowodowac bledy Parity1/Parity2 (Wymaganie 2).
// Slave Break: trzeba czekac na cisze ~8 ms (BREAK_SILENCE_US) przed jego wystawieniem.
// Zmiana tych wartosci wymaga ponownego strojenia pod konkretny radio.

// --- SKANOWANIE PRZEWIJANIEM (FF/REW) ---
// CDX-M670 wysyla DOKLADNIE JEDNA ramke 0x24/0x25 na nacisniecie (potwierdzone
// logiem [SEEKDBG]: jedna ramka, len=6, brak powtorzen przy przytrzymaniu i brak
// sygnalu zwolnienia). Nie da sie wiec wykryc "trzymania". Zamiast tego stosujemy
// SKANOWANIE ZATRZASKOWE: nacisniecie FF/REW startuje skanowanie (co SEEK_REPEAT_MS
// skok o SEEK_STEP_SEC, slyszalny podglad), ponowne nacisniecie tego samego
// kierunku je zatrzymuje, a przeciwny — odwraca. Bezpiecznik SEEK_SCAN_MAX_MS
// konczy skan, gdyby uzytkownik zapomnial go zatrzymac.
constexpr unsigned long SEEK_REPEAT_MS   = 400;
constexpr unsigned long SEEK_SCAN_MAX_MS = 30000;

// --- ADRESY UNILINK ---
constexpr uint8_t ADDR_GROUP_CD  = 0x30;   // grupa CD, brak ID (start/reset)
// [DEVIATION §5/§6] ADDR_DEFAULT zmienia znaczenie: start z adresu grupowego
// 0x30 (grupa CD, brak ID) wg Kompendium §5/§6, zamiast sztywnego 0x31.
// Bylo: constexpr uint8_t ADDR_DEFAULT = 0x31;
constexpr uint8_t ADDR_DEFAULT   = ADDR_GROUP_CD;  // adres zrodlowy przed przydzialem ID
constexpr uint8_t ADDR_BROADCAST = 0x18;   // adres rozglosza (ANYONE? itp.)
constexpr uint8_t ADDR_MASTER    = 0x10;   // TAD radia (ramki OD mastera)
constexpr uint8_t ADDR_DISPLAY   = 0x14;   // TAD procesora ekranu radia

// --- STROJENIE CZASOW MASZYNY STANOW (ms) ---
constexpr unsigned long INIT_DURATION_MS = 800;  // 0xC0 -> 0x80 (po pierwszym PINGu)
constexpr unsigned long LOAD_DURATION_MS = 50;   // 0x40 -> 0x20 (krotki, by uniknac migania LOAD)
constexpr unsigned long SEEK_DURATION_MS = 50;   // 0x20 -> 0x00 (krotki, by uniknac migania LOAD)

// --- STROJENIE SLAVE BREAK ---
constexpr unsigned long BREAK_INTERVAL_MS = 150;   // min. odstep dla PUSH przy zmianie
// Keepalive odswiezania ekranu: gdy radio nie pobralo naszego ekranu (01 13)
// dluzej niz tyle, wystawiamy break, by je obudzic. Tez min. odstep miedzy
// takimi breakami => maks. ~1000/te ms breakow/s. Czas zmienia sie 1x/s, wiec
// ~2Hz odswiezania wystarcza na plynny licznik. NIE schodzic za nisko (~150ms)
// — to byl sztorm, ktory dawal SYSTEM RESET.
// [TUNING/STABILNOSC] Podniesione 500 -> 1000 ms. Agresywny keepalive (~2 Hz
// breakow gdy radio nie odpytuje) razem z breakiem co sekunde dawal ~2.5-3 Hz
// breakow, ktore co jakis czas kolidowaly z ruchem radia -> poszarpane ramki ->
// radio robilo pelne re-discovery (crash). Lagodniejszy keepalive = mniej
// breakow = stabilniej. Czas moze byc bardziej "skokowy", ale magistrala
// nie wariuje. Aby wrocic do plynniejszego (ryzykownego) odswiezania: zmniejsz.
constexpr unsigned long DISPLAY_KEEPALIVE_MS = 1000;
constexpr unsigned long READ_SILENCE_US   = 5000;  // cisza po ktorej przetwarzamy odebrana ramke
// BREAK_SILENCE_US: wymagana CISZA przed Break. MUSI byc TUZ POWYZEJ przerwy
// command->ack radia (~6ms w sniffie). Przy 6000us lapalismy moment nadejscia
// ACK -> kolizja -> SYSTEM RESET. Przy 8000us ACK juz przyszedl. Za duzo (12000)
// = break sie nie wyzwala (czarny ekran).
// [HIGH-RISK] BREAK_SILENCE_US (8000us): timing krytyczny dla unikania kolizji
// na magistrali. Zmiana tej wartosci moze spowodowac SYSTEM RESET radia.
// Aby przywrocic wczesniejsze strojenie: zwiekszyc BREAK_SILENCE_US powyzej
// 8000us (np. 10000-12000), jezli radio nadal wykrywa kolizje.
constexpr unsigned long BREAK_SILENCE_US  = 8000;
// BREAK_HOLD_US: jak dlugo trzymac DATA LOW (na tyle, by radio wykrylo Break).
// [HIGH-RISK] BREAK_HOLD_US (2500us): timing krytyczny dla wykrycia Break przez
// radio. Zbyt krotki break nie zostanie wykryty, zbyt dlugi moze zaklocac radio.
// Aby przywrocic wczesniejsze strojenie: zmieniac BREAK_HOLD_US w zakresie
// 2000-3000us i testowac z radiem.
constexpr unsigned long BREAK_HOLD_US     = 2500;

// --- OCHRONA PRZED KOLIZJA Z URZADZENIAMI WEWNETRZNYMI RADIA ---
// Radio odpytuje swoje urzadzenia (0x3B = CD radia, 0x71 = kontroler), ktore
// odpowiadaja z opoznieniem ~9-12ms. Gdy zobaczymy poll do INNEGO urzadzenia,
// blokujemy break na to okno, az tamto zdazy odpowiedziec.
// [STABILNOSC] Przywrocone 30 ms (chwilowo bylo 15). Krotsze okno przepuszczalo
// wiecej breakow tuz po odpytaniu obcego urzadzenia, zwiekszajac ryzyko kolizji
// (obce urzadzenia odpowiadaja ~9-12 ms). 30 ms daje pewny zapas.
constexpr unsigned long FOREIGN_POLL_GUARD_MS = 30;

// --- DETEKCJA CDX-M670 ---
// Po markerze preliminary (3B/DB) ignorujemy ANYONE? przez to okno, by radio
// dokonczylo preliminary discovery bez nas (tak robi prawdziwa zmieniarka).
constexpr unsigned long PRELIMINARY_WINDOW_MS = 250;

// --- TIMEOUTY ---
constexpr unsigned long RADIO_TIMEOUT_MS = 5000;   // brak PINGa => radio zniknelo

// Zapis NVS (flash) blokuje petle na ~15-40ms. Robimy go WYLACZNIE gdy magistrala
// jest bezczynna dluzej niz ten prog (radio nie pollu­je) — nigdy w trakcie
// aktywnej wymiany, by nie opoznic odpowiedzi na radio.
constexpr unsigned long PERSIST_FLUSH_IDLE_US = 1500000;  // 1.5 s ciszy

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

// --- LOGOWANIE RAMEK (RX / TX / BREAK) — diagnostyka magistrali ---
// Lzejsze niz DEBUG_VERBOSE: loguje tylko ramki (jedna krotka linia na ramke),
// breaki i odpowiedzi na grant 0x13. Pozwala zobaczyc kadencje odpytywania
// radia i korelacje break->grant (diagnoza "czemu zegar odswieza sie co 4s").
// Wlaczone do diagnozy; wylacz po zakonczeniu, by nie obciazac petli.
constexpr bool DEBUG_FRAMES = true;

#endif // CONFIG_H
