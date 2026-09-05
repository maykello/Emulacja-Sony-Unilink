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

// --- KONFIGURACJA ZASILANIA I USB ---
constexpr uint8_t PIN_POWER_LATCH = 9; // pin trzymajacy zasilanie przetwornicy (HIGH = wlaczone)
constexpr const char* INDEX_FILE_PATH = "/unilink_index.dat"; // bufor struktury katalogow

// --- USTAWIENIA SPRZETOWE WARSTWY FIZYCZNEJ ---
constexpr bool INVERT_DATA = true;     // sprzetowy inwerter na linii DATA
constexpr int  CLOCK_EDGE  = RISING;   // zbocze zegara, na ktorym probkujemy bit

// --- ROZMIARY BUFOROW ---
constexpr int RX_BUFFER_SIZE = 64;
constexpr int TX_BUFFER_SIZE = 64;

// --- LIMITY WIRTUALNEJ ZMIENIARKI (fallback gdy nosnik nie zamontowany) ---
constexpr uint8_t MAX_TRACK_PER_DISC = 99;
constexpr uint8_t MAX_DISC           = 14;

// Kierunek przewijania przekazywany do CdChanger::seek (znak, nie dlugosc skoku
// — od dlugosci jest teraz czas trzymania klawisza, patrz SCAN_RATE* nizej).
constexpr int SEEK_STEP_SEC = 1;

// --- STROJENIE CZASOW (kompatybilne z protokolem §1) ---
// Okres bitu: ~20 µs (zgodnie z Kompendium §1). Zmiana TIMINGU BITU w przerwaniu
// onClockEvent (UnilinkBus.cpp) moze zaklocic komunikacje z radiem.
// Czas bajtu (8 bitów + przerwa): ~1 ms. Zmiana w czasie nadawania bajtu
// moze spowodowac bledy Parity1/Parity2 (Wymaganie 2).
// Slave Break musi trafic w faze HIGH fali idle (patrz sekcja BREAK_* nizej).
// Zmiana tych wartosci wymaga ponownego strojenia pod konkretny radio.

// --- SKANOWANIE PRZEWIJANIEM (FF/REW) — CUE/REVIEW JAK W ORYGINALE ---
// CDX-M670 wysyla JEDNA ramke 0x24/0x25 w chwili WCISNIECIA klawisza, a przy
// jego PUSZCZENIU broadcast `18 10 08 00`. Czas trzymania odczytujemy wiec jako
// roznice tych dwoch zdarzen (logi: FF o 21:07:00.3, `08 00` o 21:07:03.6 —
// dokladnie tyle, ile trwalo przytrzymanie).
//
// Prawdziwa zmieniarka nie skacze po rownych porcjach — przewija PLYNNIE i
// PRZYSPIESZA im dluzej trzymasz klawisz, dzieki czemu krotkie tapniecie pozwala
// dojechac precyzyjnie, a dluzsze przytrzymanie szybko przelatuje utwor.
// Modelujemy to trzema etapami predkosci (mnozniki czasu rzeczywistego):
constexpr unsigned long SCAN_PHASE1_MS = 1500;   // etap 1: precyzyjny
constexpr unsigned long SCAN_PHASE2_MS = 3000;   // etap 2: sredni (po etapie 1)
constexpr uint32_t      SCAN_RATE1     = 4;      // x4  materialu na sekunde
constexpr uint32_t      SCAN_RATE2     = 12;     // x12
constexpr uint32_t      SCAN_RATE3     = 30;     // x30 (po SCAN_PHASE1+PHASE2)
//
// SEEK_AUDIO_MS: jak czesto robimy rzeczywisty setTimeOffset w dekoderze MP3.
// To wlasnie daje slyszalne "cue": co tyle ms wskakujemy w nowe miejsce utworu.
// Kazdy skok USB+MP3 generuje lawine audio_info (INVALID_FRAMEHEADER) na Core 0;
// Serial jest wspoldzielony z Core 1, wiec zbyt czeste skoki blokowaly petle na
// tyle, ze gubilismy odpowiedzi na `01 15`. Ekran aktualizujemy plynnie (kazda
// iteracja petli), audio co SEEK_AUDIO_MS (+ finalny sync przy stopie).
constexpr unsigned long SEEK_AUDIO_MS    = 1200;
// Bezpiecznik: gdyby `08 00` nie doszlo, konczymy skan sami.
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

// --- SLAVE BREAK + ARBITRAZ (model OE ze sniffu CDX-M670) ---
// Sniff prawdziwej zmieniarki (103× `01 15`):
//   * claim przy udziale w sesji: zawsze `82 04` (0× `82 00` w trakcie burstu),
//   * BURSTY: typowo 1–4 polle, potem przerwa 0.5–9.5 s (35 luk >0.5 s),
//   * NIE ma ciaglego Request Polling @ ~23 Hz przez 15+ s.
// Emulator z always-claim non-stop trzymal sesje ~16 s, potem master ja konczyl;
// Break (Hold 3 ms) nie wznawial `01 15` (log 16:11: break=N/N, poll15=0).
// Model OE: krotka sesja claim → 1–2 granty/ekran → `82 00` konczy burst →
// po ~1 s Slave Break budzi nowa sesje.
//
constexpr unsigned long DISPLAY_REFRESH_MS = 1000;
// Break gdy sesja chce nadac, a brak `01 15` / grantu tak dlugo:
constexpr unsigned long DISPLAY_STARVED_MS = 500;
// `01 15` w tym oknie = Request Polling zywy → NIGDY Break.
// Ustawione na 3000ms: po SYSTEM RESET radio potrzebuje ~2s na discovery +
// preliminary, w tym czasie NATURALNIE nie ma `01 15`. Przy 1000ms emulator
// wyzwalal auto-recovery zanim radio zdazylo zakonczyc discovery = petla resetow.
constexpr unsigned long POLL15_ALIVE_MS    = 3000;
// Prog uzbrojenia Slave Break. Request Polling NIE jest ciagly: master prowadzi
// go tylko wtedy, gdy ktorys slave o to poprosil, po czym wraca do fali idle
// (Mictronics/Mathias Adam: "Sendet der Slave keine Slave Breaks, schaltet das
// Radio den Wechslerbetrieb ab"). CDX-M670 konczy `01 15` po ~20 s odtwarzania,
// mimo ze dalej pinguje nas Time Pollem `01 12` co ~600 ms. Brak `01 15` przez
// to okno oznacza wiec NORMALNY stan spoczynku, a nie awarie — jesli mamy co
// nadac, budzimy mastera Slave Breakiem. POLL15_ALIVE_MS (3 s) zostaje dla
// decyzji "sesja umarla" (auto-recovery), tu potrzeba znacznie krotszego progu,
// bo ekran odswiezamy ~1 Hz.
constexpr unsigned long POLL15_QUIET_BREAK_MS = 250;
// To samo okno, ale gdy oprozniamy kolejke TX (patrz BREAK_QUEUE_MIN_MS).
// Prawdziwej zmieniarce master odpowiada kolejnym `01 15` juz po ~22 ms od jej
// ramki, wiec brak pollu przez 80 ms znaczy, ze burstu nie bedzie i trzeba go
// obudzic samemu. Przy pelnych 250 ms kazda ramka bloku nazw czekala tyle
// niepotrzebnie, zanim w ogole rozwazylismy Break.
constexpr unsigned long POLL15_QUIET_DRAIN_MS = 80;
// Odstep miedzy kolejnymi Breakami. Kompromis miedzy plynnoscia ekranu a
// ryzykiem kolizji: przy 1000 ms (i BREAK_RECOVERY_MS 1500) ekran odswiezal sie
// z czestotliwoscia 0.3-0.5 Hz, przy 250/400 ms wskaznik [STAT] pokazywal juz
// 4-5 Breakow na 2 s i wrocily SYSTEM RESETy — kazdy Break to 3 ms trzymania
// magistrali, wiec im ich wiecej, tym wieksza szansa wejscia w cudza ramke.
// 500 ms wystarcza na sekundnik (potrzebny jeden Break na sekunde) i schodzi
// najwyzej do dwoch Breakow na sekunde przy oproznianiu kolejki CD-TEXT.
constexpr unsigned long BREAK_RETRY_MS     = 500;
constexpr unsigned long BREAK_RECOVERY_MS  = 600;
constexpr unsigned long BREAK_BACKOFF_MAX_MS = 3000;
// Odstep dla Breaka PILNEGO — gdy ekran radia pokazuje nieaktualna plyte/utwor/
// stan (uzytkownik wlasnie nacisnal klawisz i czeka na reakcje). Wtedy zwykly
// odstep i okno BREAK_RECOVERY_MS sa pomijane, bo kilkusekundowe opoznienie
// numeru plyty czy licznika czasu jest natychmiast widoczne na wyswietlaczu.
constexpr unsigned long BREAK_URGENT_MIN_MS = 60;
// Odstep dla Breaka, gdy w kolejce TX ZALEGAJA jeszcze ramki (blok CD-TEXT to
// 3-4 ramki, a na jeden grant idzie dokladnie jedna). Przy BREAK_RETRY_MS caly
// blok schodzil ~2 s (log: `poll15=4 break=4/4` w okienku 2 s po zmianie
// utworu), podczas gdy prawdziwa zmieniarka oddaje go w ~170 ms, bo master
// prowadzi jej burst Request Pollingu z odstepem ~22 ms.
// Nie schodzimy do BREAK_URGENT_MIN_MS: dawny sztorm Breakow bral sie z ramek
// czasu kolejkowanych OKRESOWO (kolejka byla niepusta zawsze). Teraz kolejka
// zapelnia sie tylko burstami, wiec skrocenie odstepu jest ograniczone w czasie.
constexpr unsigned long BREAK_QUEUE_MIN_MS = 150;

// Po SYSTEM RESET radio robi discovery (preliminary + ANYONE? + appoint). Caly
// cykl trwa ~2-3s. W tym czasie NIE WOLNO wyzwalac auto-recovery (`01 11`)
// ani Slave Break — radio jest W TRAKCIE normalnej procedury, nie potrzebuje
// naszej "pomocy". Grace period = 5s daje pewny zapas.
constexpr unsigned long POST_RESET_GRACE_MS = 5000;

// Model burstowy odpowiedzi: prawdziwa zmieniarka odpowiada burstami 1-4 grantow
// z przerwami 0.5-9.5s. Emulator zamyka sesje (claim `82 00`) po oddaniu tylu
// grantow i czeka na nastepna okazje (servicePositionFrame1Hz otwiera co 1s).
constexpr unsigned int MAX_BURST_GRANTS = 2;
// READ_SILENCE_US sluzy juz TYLKO jako awaryjna resynchronizacja bufora RX.
// Normalne ciecie strumienia na ramki robi UnilinkBus::readFrame po dlugosci
// wynikajacej z CMD1, wiec ta wartosc nie wplywa juz na czas odpowiedzi.
constexpr unsigned long READ_SILENCE_US   = 5000;

// --- RESYNCHRONIZACJA FAZY BITOWEJ (ISR odbioru) ---
// Przerwa miedzy zboczami zegara dluzsza niz ta wartosc oznacza poczatek nowej
// ramki i zeruje licznik bitow. Wyliczenie ze znacznikow `t=` w sniffie CDX-M670:
//   * ramka 16-bajtowa trwa 14965-15000 us -> ~937 us/bajt -> ~117 us/bit,
//     przy czym bity ida ciagiem (brak przerwy miedzy bajtami w ramce),
//   * najkrotsza zaobserwowana przerwa MIEDZY ramkami to ~5970 us.
// 1000 us lezy wygodnie miedzy tymi skalami: ~8x wiecej niz okres bitu i ~6x
// mniej niz najkrotsza przerwa miedzyramkowa.
constexpr unsigned long BYTE_RESYNC_GAP_US = 1000;

// --- SLAVE BREAK: SYNCHRONIZACJA Z FALA IDLE (§2.2 / Mictronics) ---
// Przy bezczynnej magistrali master utrzymuje na DATA fale ~8 ms LOW / ~8 ms
// HIGH. Break jest wazny WYLACZNIE w fazie HIGH, ~2 ms po zboczu w gore:
//   1) potwierdz ~8 ms LOW (idle),
//   2) czekaj 2 ms w HIGH,
//   3) sciagnij DATA LOW na ~3 ms,
//   4) pusc — reszta fazy HIGH.
// BREAK_IDLE_LOW_US: Mictronics = pelne ~8 ms LOW przed faza HIGH.
constexpr unsigned long BREAK_IDLE_LOW_US = 8000;
// BREAK_IDLE_LOW_MIN_US: spozniona probka juz na HIGH po wystarczajacym LOW.
constexpr unsigned long BREAK_IDLE_LOW_MIN_US = 6000;
constexpr unsigned long BREAK_SETTLE_US   = 2000;
constexpr unsigned long BREAK_HOLD_US     = 3000;
// Trzymaj pelne 3 ms — nie przerywaj na wczesnym zegarze (wypelniacz idle HIGH
// SophWiki: 8 bit clock w fazie HIGH). Puszczamy dopiero po HOLD_US.
constexpr unsigned long BREAK_MIN_VISIBLE_US = 3000;
// BREAK_ARM_TIMEOUT_US: jak dlugo czekamy na czysta fale idle po uzbrojeniu.
// Time Poll CDX-M670 ~600 ms — 300 ms bylo za krotkie (log: break=N/0, Hold
// nigdy nie startowal). 2.5 s obejmuje kilka cykli keepalive.
constexpr unsigned long BREAK_ARM_TIMEOUT_US = 2500000;

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

// --- CD-TEXT ---
// Jak czesto powtarzamy komplet nazw (utwor 0xD2 + plyta 0xDA) z wlasnej
// inicjatywy. Zrzut prawdziwej zmieniarki pokazuje, ze robi to PRAKTYCZNIE BEZ
// PRZERWY — komplet nazw leci co ~1-2 s przez cale odtwarzanie:
//   16:58:32.0, :34.1, :35.1, :37.2, :39.3, :51.2, :53.1, :54.7, :56.6, :57.5
// CDX-M670 restartuje przewijanie marquee dopiero na kompletnym bloku nazw,
// wiec przy dawnych 30 s tekst przewijal sie raz i zamieral (a po zmianie
// zrodla albo utracie sesji ekran zostawal pusty do nastepnego okresu).
constexpr unsigned long CD_TEXT_REPEAT_MS = 2000;

// --- CD-TEXT TIME FLASH (okresowe pokazywanie timera zamiast nazwy) ---
// Podczas odtwarzania z CD-TEXT radio pokazuje nazwe utworu/plyty. Co jakis czas
// emulator chwilowo wycofuje flage CD-TEXT, zeby radio przeszlo na widok timera
// (czas odtwarzania) — dokladnie tak, jak robi to prawdziwa zmieniarka.
// INTERVAL = co ile sekund pojawia sie flash timera.
// DURATION = jak dlugo timer jest widoczny zanim wroci CD-TEXT.
// Ustaw INTERVAL na 0, zeby WYLACZYC te funkcje (CD-TEXT widoczny non-stop).
constexpr unsigned long CDTEXT_TIME_FLASH_INTERVAL_MS = 10000;  // co 10s
constexpr unsigned long CDTEXT_TIME_FLASH_DURATION_MS = 4000;   // na 4s

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
// WYLACZONE domyslnie: przy ~23 pollach/s Serial.printf w petli glownej
// konkuruje z taskiem audio o UART i w trakcie seeku powodowal gubienie
// odpowiedzi `01 15` (zamrozony ekran). Wlacz tylko na krotka diagnoze.
constexpr bool DEBUG_FRAMES = false;

#endif // CONFIG_H
