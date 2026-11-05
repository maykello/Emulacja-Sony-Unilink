# Sony Unilink — kompendium protokołu i emulatora zmieniarki

Dokumentacja magistrali Sony Unilink w takiej postaci, w jakiej działa
**emulator zmieniarki CD na ESP32-S3** (`ESP32/Emulator`) z radiem
**Sony CDX-M670**. Opis jest zsynchronizowany z kodem (listopad 2026).

Źródła, w kolejności wiarygodności:
1. **Kod emulatora** i jego zachowanie w aucie (crash logi `CrashLogs/`, logi
   `Python logger/unilink_log_emulator_*`).
2. **Sniff prawdziwej zmieniarki** z CDX-M670
   (`Python logger/unilink_log_zmieniarka_CDX-M670_20260605_144705.txt`
   i `..._165726.txt`).
3. **Mictronics „Inside Sony Unilink”** i tablica komend `command.htm`.
4. Emulatory open-source (Becker/AVR — Michael Wolf, STM32 — deividAlfa).

Oznaczenia:
- **[POTW]** — potwierdzone sniffem prawdziwej zmieniarki i działaniem emulatora.
- **[EMUL]** — zachowanie emulatora; działa z CDX-M670, ale to nasz wybór
  implementacyjny (bywa świadomym odstępstwem od oryginału).
- **[WNIOSEK]** — wywnioskowane z logów, prawdopodobne.
- **[ZEWN]** — tylko ze źródeł zewnętrznych, niesprawdzone na CDX-M670.

Rozdział 17 zawiera listę poprawek względem poprzedniej wersji tego dokumentu.

---

## Spis treści
1. [Sprzęt emulatora](#1-sprzęt-emulatora)
2. [Warstwa fizyczna](#2-warstwa-fizyczna)
3. [Stany magistrali i Slave Break](#3-stany-magistrali-i-slave-break)
4. [Struktura ramki i sumy kontrolne](#4-struktura-ramki-i-sumy-kontrolne)
5. [Adresy na magistrali CDX-M670](#5-adresy-na-magistrali-cdx-m670)
6. [Discovery, przydział adresu, rejestracja](#6-discovery-przydział-adresu-rejestracja)
7. [Cykl pracy: ping, zgłoszenie, grant](#7-cykl-pracy-ping-zgłoszenie-grant)
8. [Bajt statusu](#8-bajt-statusu)
9. [Ramki ekranu: 0xC0, 0x90, 0x8E](#9-ramki-ekranu-0xc0-0x90-0x8e)
10. [CD-TEXT](#10-cd-text)
11. [Magazynek, informacje o płycie, ikony](#11-magazynek-informacje-o-płycie-ikony)
12. [Komendy od radia i reakcje emulatora](#12-komendy-od-radia-i-reakcje-emulatora)
13. [Przewijanie FF/REW](#13-przewijanie-ffrew)
14. [Zasilanie, BUS_ON i cykl życia sesji](#14-zasilanie-bus_on-i-cykl-życia-sesji)
15. [Odporność na błędy i diagnostyka](#15-odporność-na-błędy-i-diagnostyka)
16. [Nośnik USB i odtwarzanie](#16-nośnik-usb-i-odtwarzanie)
17. [Poprawki względem poprzedniej wersji](#17-poprawki-względem-poprzedniej-wersji)
18. [Załącznik: zdekodowane przykłady](#18-załącznik-zdekodowane-przykłady)

---

## 1. Sprzęt emulatora

[EMUL]
| Element | Realizacja |
|---|---|
| MCU | ESP32-S3 (moduł 16 MB flash, 8 MB PSRAM), 240 MHz, Arduino-ESP32 2.0.17 (IDF 4.4) |
| BUS_ON | GPIO 4, przez dzielnik napięcia |
| CLOCK | GPIO 5, przez level shifter; szeregowo ~1 kΩ, zener 5V6 do masy |
| DATA | GPIO 6, przez level shifter; szeregowo 48 Ω (większe nie działały), zener 5V6 |
| Podtrzymanie zasilania | GPIO 9 (`PIN_POWER_LATCH`, HIGH = przetwornica włączona) |
| Audio | PCM5102A po I2S (BCK 7, LRCK 15, DIN 13), wyciszenie XSMT na GPIO 16 |
| Nośnik | pendrive przez USB Host (FAT32) |

Na pinie DATA poziom **dominujący (logiczna 1) = LOW** (`INVERT_DATA = true`).
Emulator nigdy nie wystawia stanu wysokiego. Nadaje, włączając wyjście z
zatrzaśniętym LOW; zwalnia linię, wyłączając wyjście (styl open-drain).

Podział pracy:
| Wątek | Rdzeń | Zadanie |
|---|:--:|---|
| przerwanie zegara (IRAM, poziom 3) | 1 | odbiór i nadawanie bitów, szybkie odpowiedzi |
| zadanie `unilink` (prio 20) | 1 | protokół, logika zmieniarki |
| `loop()` (prio 1) | 1 | zasilanie, zapisy NVS/LittleFS, crash logi |
| zadanie audio (prio 2) | 0 | dekodowanie MP3 → I2S |
| zadania USB | 0 | stos USB Host, system plików |

---

## 2. Warstwa fizyczna

[POTW] Zmierzone na CDX-M670:

- **Zegar generuje wyłącznie master (radio)**, także dla bitów nadawanych przez
  zmieniarkę.
- **Okres bitu ≈ 125 µs**. Bit próbowany jest na **zboczu narastającym** CLOCK.
- Bajty idą jeden za drugim bez przerw, **~1 ms na bajt**, najstarszy bit pierwszy.
- Między ramkami jest **~5 ms ciszy zegara**.
- **Po każdej ramce master taktuje slot odpowiedzi** — zaczyna go więcej niż 1 ms
  po ostatnim bicie ramki. Jeśli nikt nie odpowiada, master czyta w slocie
  pojedynczy bajt `0x00`.

Wnioski dla odbiornika [EMUL]:
- Przerwa w zegarze **> 1 ms** (`BUS_FRAME_GAP_US`) = początek nowej ramki albo slotu.
- Zbocza bliżej niż **5 µs** od poprzedniego to zakłócenie (`glitch`); w trakcie
  naszego nadawania i Breaka próg to **30 µs** (§15.3).
- Po zboczu przerwanie sprawdza, czy CLOCK nadal jest wysoki. Jeśli nie, to szpilka
  (`spike`) i zbocze jest ignorowane.
- Pierwszy bajt ramki `< 0x10` albo `0xFF` nie jest początkiem ramki (pusty slot,
  śmieci).

Odpowiedź w slocie [EMUL]:
- Musi być **uzbrojona, zanim master zacznie taktować slot**. Termin uzbrojenia
  przez zadanie to `BUS_RESPONSE_DEADLINE_US` = 4 ms od końca ramki. Później nie
  nadajemy w ogóle, bo spóźniona odpowiedź wpadłaby w następną ramkę mastera i
  radio zrobiłoby reset magistrali.
- Wyjście włączane jest na pierwszym takcie slotu, z już ustawionym poziomem.
- Jeśli master przestanie taktować w trakcie nadawania (przerwa > 2 ms) albo nie
  zacznie slotu w ciągu 9 ms, linia jest natychmiast zwalniana.

---

## 3. Stany magistrali i Slave Break

[POTW]/[ZEWN Mictronics]
- **off** — BUS_ON = 0.
- **idle** — BUS_ON = 1, zegar stoi, a master trzyma na DATA falę: ~8 ms
  dominujący, ~8 ms recesywny.
- **active** — trwa transmisja taktowana zegarem.

### 3.1. Slave Break — zgłoszenie chęci nadawania
[POTW] Slave, który ma coś do wysłania, w stanie idle:
1. czeka na fazę dominującą fali trwającą **≥ 6 ms**,
2. po przejściu w fazę recesywną odczekuje **2 ms**,
3. ściąga DATA na **3 ms** (impuls musi leżeć w fazie recesywnej, inaczej master
   go nie widzi),
4. zwalnia linię.

Master odpowiada pollem **`18 10 01 15`** po ~11–14 ms (patrz §7.2).

Parametry emulatora (`Config.h`) [EMUL]:
| Parametr | Wartość | Znaczenie |
|---|:--:|---|
| `BREAK_QUIET_BEFORE_US` | 8 ms | cisza zegara przed próbą |
| `BREAK_SEARCH_MAX_US` | 40 ms | maks. czekanie na falę idle |
| `BREAK_IDLE_LOW_MIN_US` / `MAX_US` | 6 / 30 ms | akceptowana faza dominująca |
| `BREAK_SETTLE_US` | 2 ms | opóźnienie w fazie recesywnej |
| `BREAK_HOLD_US` | 3 ms | długość impulsu |
| `BREAK_POLL_WAIT_MS` | 60 ms | czekanie na `18 10 01 15` |
| `FOREIGN_POLL_GUARD_MS` | 30 ms | brak Breaka po zapytaniu do innego urządzenia |

Jeśli w trakcie impulsu pojawi się zbocze zegara, przerwanie **natychmiast
zwalnia DATA**, żeby nie zepsuć ramki mastera.

> Mictronics opisuje też budzenie mastera przez slave. Emulator go nie używa,
> bo radio samo włącza BUS_ON.

---

## 4. Struktura ramki i sumy kontrolne

[POTW] Długość ramki wynika z **CMD1** (trzeci bajt):

| CMD1 | Długość | Nazwa |
|---|:--:|---|
| `< 0x80` | 6 B | short |
| `0x80–0xBF` | 11 B | middle |
| `≥ 0xC0` | 16 B | long |

```
short  (6):  RAD TAD CMD1 CMD2 P1 00
middle (11): RAD TAD CMD1 CMD2 P1 D1 D2 D3 D4 P2 00
long   (16): RAD TAD CMD1 CMD2 P1 D1 D2 D3 D4 D5 D6 D7 D8 D9 P2 00
```
- **RAD** — odbiorca, **TAD** — nadawca.
- **P1** = `(RAD + TAD + CMD1 + CMD2) & 0xFF`.
- **P2** = `(P1 + D1 + … + Dn) & 0xFF`.
- Końcowy `00` nie wchodzi do żadnej sumy.

Emulator odrzuca ramki ze złym P1, złym P2 albo bez końcowego `00` (wpis `BAD`
w logu).

Notacja w tym dokumencie: w ramce long `D1…D9` to bajty 5–13 (indeks od 0),
`P2` to bajt 14.

---

## 5. Adresy na magistrali CDX-M670

[POTW] (z ruchu w logach)
| Adres | Rola |
|:--:|---|
| `0x10` | master (radio) |
| `0x14` | procesor ekranu radia — **też pinguje zmieniarkę** (`31 14 01 12`), wydaje granty (`31 14 01 13`), robi polle (`18 14 01 15`) |
| `0x11` | klawiatura — komendy transportu przychodzą z TAD = `0x11` (np. `31 11 26 10`) |
| `0x18` | broadcast |
| `0x30` | grupa zmieniarek CD — adres przed przydziałem; też adres zarządcy grupy w rejestracji (§6.5) |
| `0x31…0x3F` | przydzielony adres zmieniarki (zwykle `0x31`) |
| `0x3B` | wewnętrzne urządzenie grupy CD w radiu |
| `0x70` | ekran — tu idą ramki do wyświetlenia |
| `0x77` | ekran, wariant RAD dla ramki `0xC0` w trakcie zmiany płyty lub utworu |
| `0x71` | kontroler ekranu — prosi zmieniarkę o tekst (`31 71 84 D7 …`) |
| `0x90` | odbiorca ramki `0x9C` (zmiana płyty) |
| `0x91`, `0x51` | DSP i tuner (ruch wewnętrzny, emulator go ignoruje) |

[ZEWN Mictronics] Grupy (górny nibble): `0x3` CD, `0x5` tuner, `0x6` kaseta,
`0x7` ekran, `0x8` multiplekser, `0xC` zegar, `0xD` MD. Dolny nibble `0` =
broadcast do grupy.

**Adres zmieniarki jest dynamiczny** — przydziela go radio (§6). Nie można
zakładać `0x31` na sztywno.

---

## 6. Discovery, przydział adresu, rejestracja

### 6.1. Sekwencja radia
[POTW]
1. Przy włączaniu CDX-M670 robi discovery urządzeń wewnętrznych przy **BUS_ON = 0
   przez ~220 ms**. Na starcie BUS_ON potrafi więc kilka razy zgasnąć i wrócić.
2. **`18 10 01 02`** — „Anyone?”.
3. Odpowiedź zmieniarki bez adresu — device info (§6.2), TAD = `0x30`.
4. **Appoint** `3X 10 02 YY` — RAD = nadawany adres. **Dolny nibble `YY` to maska
   zgłoszeń** używana później w arbitrażu `01 15` (np. `31 10 02 14` → adres
   `0x31`, maska `0x04`).
5. Zmieniarka potwierdza tym samym device info już z nowym TAD.
6. Reset magistrali: **`18 10 01 00`** — wszystkie adresy tracą ważność, radio
   zaczyna discovery od nowa.

### 6.2. Device info `0x8C`
[POTW] Ramka 1:1 ze sniffu oryginału:
```
10 30 8C D0 9C 05 A8 1F A3 0B 00    ; przed przydziałem (TAD 0x30)
10 31 8C D0 9D 04 A8 1F A3 0B 00    ; po appoint (TAD 0x31)
```
D1 dobierane jest jako `0xA1 − P1`, żeby P2 zawsze wynosiło `0x0B`. Tak robi
oryginał i tak robi emulator (`buildDeviceInfo`).

### 6.3. Zachowanie emulatora w discovery
[EMUL]
- **Anyone?** — odpowiada device info z TAD `0x30`, o ile nie ma potwierdzonego
  adresu. Adres przejęty z NVS albo przydział starszy niż 2 s jest wtedy
  unieważniany.
- **Appoint** — przyjmowany, jeśli przyszedł w ciągu 300 ms od naszej odpowiedzi
  na Anyone? albo jest skierowany pod zapamiętany adres. Po nim emulator:
  odpowiada device info, zapamiętuje adres i maskę, kolejkuje rejestrację (§6.5),
  a adres zapisuje w NVS (3 s po appoint, w oknie zapisu).
- **Reset `18 10 01 00`** — kasuje adres i kolejkę nadawczą. Reset jest honorowany
  także przy BUS_ON = 0.

### 6.4. Adres z poprzedniej sesji (NVS)
[EMUL]/[POTW] Radio włączone na źródle CD często **pinguje znaną zmieniarkę bez
discovery**.
- Emulator po starcie odpowiada na ping pod adresem zapamiętanym w NVS.
- Każda ramka od radia pod ten adres (ping, `F0 3X`, komenda) przejmuje go bez
  appoint.
- Krótki zanik BUS_ON nie kasuje adresu — kasuje go dopiero `18 10 01 00`.

### 6.5. Rejestracja w zarządcy grupy CD
[POTW] Po appoint oryginał wysyła (przez Break i grant) dwie ramki do `0x30`:
```
30 31 70 A0 71 00    ; możliwości: zmieniarka CD z CD-TEXT
30 31 70 10 E1 00    ; gotowość: napęd online
```
Emulator robi to samo.

### 6.6. `18 10 01 11` i ramka `10 18 04 00`
[POTW] Radio wysyła `18 10 01 11`, pytając o nieprzydzielone urządzenia.
- Emulator bez adresu odpowiada **`10 18 04 00 2C 00`** — maksymalnie raz na
  1,5 s i 6 razy z rzędu, potem 30 s przerwy.
- **Radio reaguje na nią pełnym resetem magistrali** (~85 ms później) i nowym
  discovery. Kosztuje to jeden cykl BUS_ON, ale jest jedyną drogą, gdy radio
  zapomniało o zmieniarce.

### 6.7. Radio zapomniało o zmieniarce
[EMUL] Jeśli przez **15 s** nie przyszedł ping pod nasz adres, a na magistrali
jest ruch, emulator porzuca adres i wraca do nasłuchu discovery.

---

## 7. Cykl pracy: ping, zgłoszenie, grant

### 7.1. Ping `3X 10 01 12` → status
[POTW]
- Master pinguje przydzieloną zmieniarkę **co ~2,5 s**.
- Często **~28 ms później pinguje ją też procesor ekranu**: `3X 14 01 12`.
- Odpowiedź w slocie: `TAD 3X 00 STATUS P1 00`, gdzie TAD to nadawca pingu.
  Przykład: `10 31 00 00 41 00`.

[POTW] **Reguła radia — brak odpowiedzi na ping kończy się resetem magistrali:**
- **Ping mastera bez odpowiedzi** → radio ponawia go po **~600 ms** (zamiast
  2,5 s). Drugi brak odpowiedzi → **`18 10 01 00`**.
- **Ping procesora ekranu** (`3X 14 01 12`) bez odpowiedzi → reset w ciągu
  **~75 ms** (widziane dwa razy).

Dlatego odpowiedź na ping jest w emulatorze najważniejszą rzeczą na magistrali
(§15.1).

### 7.2. Zgłoszenie i grant
[POTW] Gdy zmieniarka ma ramkę do wysłania:
```
(Slave Break)
18 10 01 15 3E 00                    ; master: kto chce nadawać?
10 18 82 MM P1 00 00 00 00 P1 00     ; zmieniarka: maska MM (z appoint), P2 = P1
31 10 01 13 55 00                    ; master: grant dla 0x31 (bywa też 31 14 01 13)
70 31 90 30 …                        ; zmieniarka: ramka w slocie grantu
```
- Maski kilku urządzeń sumują się na linii (wired-OR). Master na tej podstawie
  przydziela granty po kolei.
- Na jeden grant idzie **jedna ramka**.
- Master robi też `18 10 01 15` sam z siebie, bez Breaka.

[EMUL] W emulatorze przygotowaną ramkę trzyma **przerwanie**. Na `18 10 01 15`
samo wysyła zgłoszenie, a na grant samo wysyła ramkę (§15.1).

### 7.3. Inne `01 xx` od mastera
| CMD2 | Znaczenie |
|:--:|---|
| `00` | reset magistrali |
| `02` | Anyone? |
| `11` | pytanie o nieprzydzielone urządzenia (§6.6) |
| `12` | ping |
| `13` | grant |
| `15` | poll zgłoszeń |

[ZEWN] Mictronics wymienia też `03` (Anyone special) i `04` (Appoint end).

---

## 8. Bajt statusu

Status idzie w odpowiedzi na ping i w ramce `0xC0` (bajt CMD2).

| Kod | Znaczenie | Kiedy wysyła emulator |
|:--:|---|---|
| `0x00` | Playing | gra; **także w trakcie FF/REW** [EMUL, odstępstwo] |
| `0x20` | Changed CD | po zmianie płyty, przed graniem (≥ 250 ms) |
| `0x21` | Seeking | **nie używany** — CDX-M670 gasi wtedy licznik czasu (`--:--`) |
| `0x40` | Changing / ładowanie | ładowanie utworu lub płyty (≥ 80 ms) |
| `0x80` | Idle | gotowy, nie gra; też w stanie Init [EMUL] |
| `0xC0` | Ejecting | nie używany |

Kolejność przy starcie utworu [EMUL]: `0x40` → (`0x20` przy zmianie płyty) →
`0x00`. Każde przejście czeka, aż radio zobaczy stan w co najmniej jednym pingu
lub granie (maks. 500 ms).

---

## 9. Ramki ekranu: 0xC0, 0x90, 0x8E

**Zegar utworu prowadzi zmieniarka.** Radio wyświetla to, co dostanie. Gdy
ramki czasu przestają przychodzić, licznik na ekranie stoi.

### 9.1. `0x90` — czas, raz na sekundę
[POTW]
```
70 31 90 30 61 F1 F0 05 8A D1 00
            P1 TR MM SS DF P2
```
| Bajt | Wartość | Znaczenie |
|:--:|---|---|
| CMD2 | `30` | stały |
| D1 | `F1` | **numer utworu**, BCD z dopełnieniem F (patrz niżej) |
| D2 | `F0` | **minuty**, BCD z dopełnieniem F |
| D3 | `05` | **sekundy**, zwykłe BCD |
| D4 | `8A` | **górny nibble = numer płyty** (8), dolny = flagi CD-TEXT (§9.4) |

**BCD z dopełnieniem F:** wartości 0–9 mają górny nibble `F` (7 → `F7`), a
wartości ≥ 10 to zwykłe BCD (12 → `12`).

Emulator wysyła `0x90` przy każdej zmianie sekundy, nie częściej niż co 250 ms.

### 9.2. `0xC0` — pełny status
[POTW] Wysyłana przy zmianie utworu, płyty, stanu albo flag CD-TEXT, a także w
stanach przejściowych:
```
70 31 C0 00 61 00 00 00 00 30 F1 F0 00 88 FA 00    ; gra TR1 0:00, CD8
77 31 C0 20 88 00 00 00 00 30 F2 FF FF 88 30 00    ; zmiana utworu, czas nieznany
77 31 C0 40 A8 00 00 00 00 00 F1 F0 00 81 1A 00    ; zmiana płyty
```
| Bajt | Znaczenie |
|:--:|---|
| RAD | `70`; **`77`** w stanach `0x20`/`0x40` |
| CMD2 | status (§8) |
| D1–D4 | `00` |
| D5 | `30` = płyta obecna; `00` przy zmianie płyty albo braku płyty |
| D6 | numer utworu (BCD z dopełnieniem F) |
| D7 | minuty (BCD F); `FF` = czas nieznany; `F0` przy zmianie płyty |
| D8 | sekundy (BCD); `FF` = czas nieznany; `00` przy zmianie płyty |
| D9 | numer płyty w górnym nibblu, dolny = flagi; przy zmianie płyty `1` (płyta obecna) albo `0` |

Czas „nieznany” (`FF FF`) wysyłany jest poza stanami Playing. Radio pokazuje
wtedy `--:--`.

### 9.3. `0x8E` — zmieniarka w spoczynku
[POTW] `70 31 8E 80 AF 90 00 00 D0 P2 00`:
- CMD2 = `80`,
- D1 = `90`,
- D4 = numer płyty w górnym nibblu.

Wysyłana jednorazowo po wejściu w Idle.

### 9.4. Flagi CD-TEXT w dolnym nibblu numeru płyty
[POTW] Sekwencja oryginału przy każdym nowym utworze:

| Etap | Flaga | Co się dzieje |
|:--:|:--:|---|
| 0 | `0x8` | pierwsza `0xC0`, potem pierwszy blok tekstu (`D2` + `DA`) |
| 1 | `0xB` | druga `0xC0`, potem powtórzony blok (`D2` + `DA` + `D7`) |
| 2 | `0xA` | trzecia `0xC0`; kontroler ekranu `0x71` prosi wtedy o `84 D7` |

Gdy płyta jest pusta, flaga = `0`. Emulator powtarza całą sekwencję przy każdym
utworze.

---

## 10. CD-TEXT

### 10.1. Wariant CDX-M670: `0xD2` / `0xDA` / `0xD7`
[POTW] CDX-M670 wyświetla nazwy w ramkach long po **6 znaków**. Pierwszy znak
siedzi w CMD2:
```
70 31 D2 4B BE 61 70 69 74 61 02 00 F1 10 D0 00
         K     a  p  i  t  a  |  |  |  |
                              |  |  |  +- D9 = 0x10 (stały)
                              |  |  +---- D8 = numer utworu (BCD F); w 0xDA numer płyty (1–14)
                              |  +------- D7: 0x00, a w ostatnim segmencie 0x01
                              +---------- D6: 0x02 = ciąg dalszy, 0x00 = ostatni segment
```
| CMD1 | Treść | D8 |
|:--:|---|---|
| `0xD2` | nazwa utworu | numer utworu (BCD F) |
| `0xDA` | nazwa płyty | numer płyty |
| `0xD7` | znacznik końca bloku: znaki `00`, D6 = `02`, D8 = `00`, D9 = **`1C`** | — |

[EMUL]
- Nazwa utworu ma maks. **12 znaków** (`CDTEXT_D2_MAX_CHARS`), czyli 2 ramki.
  Nazwa płyty to 1 ramka (6 znaków).
- Pusta nazwa daje jedną ramkę ze znakami `00` i D6 = `02`.
- Bloki tekstu idą w cyklu flag z §9.4. Tylko w stanie Playing, gdy radio
  potwierdziło już ekran z czasem.
- Gdy w kolejce czeka ramka ekranu (`0xC0`/`0x90`), ma ona pierwszeństwo przed
  tekstem. Dzięki temu licznik nie gubi sekund przy wysyłaniu nazw.

### 10.2. Żądania tekstu `84 xx`
[POTW]/[EMUL] Radio albo kontroler ekranu `0x71` wysyła `3X TAD 84 CMD2 …`:
| CMD2 | Odpowiedź emulatora |
|:--:|---|
| `D7` | D4 = `00` → blok nazwy płyty (`DA` + `D7`); inaczej blok nazwy utworu albo samo `D7`, jeśli cykl flag jest zakończony |
| `D2` | blok nazwy utworu |
| `DA`, `DD` | blok nazwy płyty (numer płyty z D1, jeśli podany) |
| `D9` | pole 8-znakowe nazwy utworu (`C9` dla pól 0–1, `D9` dla 2–5); numer pola w D1 |
| `95` | mapa magazynka (§11.1) |
| `97` | info o płycie (§11.2) |

Żądanie `84` nie oznacza, że radio wybrało zmieniarkę jako źródło.

### 10.3. Wariant klasyczny (8 znaków) — tylko na żądanie `84 D9`
[ZEWN]/[EMUL] Pola po 8 znaków:
- nazwa utworu: `C9` (pola 0–1) i `D9` (pola 2–5),
- nazwa płyty: `CD` (pola 0–1) i `DD` (pola 2–5).

Znaki są w CMD2 i D1…D7, D8 = `00`, D9 = numer pola. Brak tekstu w danym polu =
brak odpowiedzi.

### 10.4. Przygotowanie tekstu
[EMUL] `CdText::sanitizeAscii`:
- obcina rozszerzenie pliku i prefiks numeru (`01. `, `01 - `, `01_`),
- zamienia polskie znaki na ASCII (ą → a…) i `_` na spację,
- zwija podwójne spacje i usuwa znaki spoza 0x20–0x7E.

Tryby specjalne:
- **Błąd** (Repeat wyłączony): nazwa płyty `ERROR`, nazwa utworu = tekst błędu,
  np. `NO PENDRIVE`, `FAT32 ERR`, `EMPTY CD`; licznik 99:01–99:10.
- **Repeat One/All**: tryb „OBD” — tekst odświeżany co 1 s.

---

## 11. Magazynek, informacje o płycie, ikony

### 11.1. `0x95` — mapa obecnych płyt
[POTW] `70 31 95 CMD2 P1 D1 00 00 D4 P2 00`. **Bity są przeplecione:**
| Bit | CMD2 | D1 |
|:--:|:--:|:--:|
| 4 / 5 / 6 / 7 | CD1 / CD2 / CD3 / CD4 | CD9 / CD10 / CD11 / CD12 |
| 0 / 1 / 2 / 3 | CD5 / CD6 / CD7 / CD8 | CD13 / CD14 / — / — |

D4 = numer bieżącej płyty w górnym nibblu, dolny = `0xE` (14 slotów).
Przykład ze sniffu: `70 31 95 08 3E 00 00 00 8A C8 00` = obecna tylko płyta 8.

[EMUL] Płyta jest obecna, gdy jej folder ma pliki audio. Gdy pendrive jest
podpięty, ale indeks się jeszcze buduje, mapa zawiera płytę bieżącą — żeby radio
nie widziało pustego magazynka.

### 11.2. `0x97` — informacje o płycie
[POTW] `70 31 97 01 P1 TT MM SS D4 P2 00`:
- TT = liczba utworów (BCD),
- MM:SS = czas całkowity (BCD),
- D4 = numer płyty w górnym nibblu.

[EMUL] Czas całkowity jest syntetyczny, wyliczany z numeru płyty — spójny z
`0xC5`/`0xD5`. Pusta płyta = 99 utworów i czas 00:00.

### 11.3. `0x9C` — zmiana płyty
[POTW] `90 31 9C 00 P1 00 00 00 D4 P2 00`. D4 = numer płyty w górnym nibblu,
dolny `8` = płyta obecna (`0` = brak). Przykład: `90 31 9C 00 5D 00 00 00 88 E5 00`
= CD8.

### 11.4. `0xC5` / `0xD5` — identyfikator płyty
[POTW] Do mastera `0x10`:
```
10 31 C5 A2 A8 24 77 52 F0 00 00 00 00 88 0D 00    ; po skanie magazynka
10 31 D5 A2 B8 24 77 52 F1 00 00 00 00 88 1E 00    ; zmiana płyty w trakcie grania
```
| Bajt | Znaczenie |
|:--:|---|
| CMD2 | `A` + setne sekundy |
| D1 | liczba utworów (BCD) |
| D2, D3 | minuty i sekundy (BCD) |
| D4 | `F0` (C5) / `F1` (D5) |
| D5–D8 | `00` |
| D9 | numer płyty w górnym nibblu, dolny `8` |

Radio używa tego jako klucza płyty. [EMUL] Wartości są deterministyczne per
numer płyty (FNV), więc stałe między sesjami.

### 11.5. `0x94` — ikony trybów
[POTW]/[EMUL] `70 3X 94 00 P1 D1 D2 D3 00 P2 00`:
- D1: dolny nibble `8`/`9`/`A` = Repeat off / one / all; górny nibble `2`/`3` =
  Intro off / on,
- D2 = `80` (Shuffle off) / `D0` (Shuffle on),
- D3 = `80` (Bank off).

### 11.6. Co emulator wysyła po zmianie płyty
[EMUL] `0x9C` + `0xD5`, potem `0xC0` ze statusem `0x40`/`0x20` i nowy CD-TEXT.

---

## 12. Komendy od radia i reakcje emulatora

Komendy przychodzą pod nasz adres od `0x10`, `0x14` albo `0x11` (klawiatura).
Liczy się RAD i CMD1, nie TAD.

| Ramka | Znaczenie | Reakcja emulatora |
|---|---|---|
| `3X .. 20 ..` | PLAY | start albo kontynuacja bez restartu utworu |
| `3X .. 24 ..` / `25` | wciśnięcie FF / REW | start przewijania (§13) |
| `18 10 08 00` | puszczenie klawisza | koniec przewijania, jeden skok audio |
| `3X .. 26 ..` / `27` | następny / poprzedni utwór | poprzedni: gdy czas > 2 s, restart utworu; na granicy płyty przejście do sąsiedniej niepustej |
| `3X .. 28 ..` / `29` | następna / poprzednia płyta | pomija puste foldery |
| `3X .. 34/35/36 ..` | Repeat / Shuffle / Intro | przełącza tryb, wysyła `0x94` |
| `3X .. 84 ..` | żądanie tekstu lub informacji | §10.2 |
| `3X .. B0 0d …` | wybór bezpośredni | płyta = dolny nibble CMD2, utwór = D1 (BCD/F; `00`/`FF` = 1) |
| `18 10 F0 3X` | radio wybrało źródło 3X | jeśli to my: start grania; inaczej stop |
| `18 1x 87 xx` | stan zasilania radia | bity 0 i 1 ustawione (`2B`, `6B`, `AB`, `EB`) = włączone; inaczej (`6A`, `2A`, `22`, `29`, `21`) = wyłączanie → stop |
| `18 10 01 00` | reset magistrali | §6.3 |

Każda komenda inna niż `84` oznacza, że radio gra ze zmieniarki
(`s_selected = true`). Ramki ekranu i CD-TEXT emulator wysyła tylko wtedy, gdy
jest wybrany.

[POTW] Sekwencja wyłączania radia: `87 6A` / `87 2A` → ~10 s → `87 22` →
BUS_ON = 0. Po wybudzeniu przez `01 11` przy zgaszonym radiu: `87 29` → ~10 s →
`87 21` → BUS_ON = 0.

---

## 13. Przewijanie FF/REW

[POTW] CDX-M670 wysyła `24`/`25` **przy wciśnięciu** klawisza, a broadcast
`18 10 08 00` **przy puszczeniu**. Nie ma ramek w trakcie trzymania.

[EMUL]
- Pozycja przesuwa się od razu o 1 s, potem z przyspieszeniem:
  - ×3 przez pierwsze 2 s,
  - ×8 przez kolejne 5 s,
  - ×16 dalej.
- Limit bezpieczeństwa to 30 s, na wypadek gdyby `08 00` nie doszło.
- Status pozostaje `0x00`, a ramki `0x90` niosą przesuwany czas (co ≥ 250 ms),
  bez przeładowania ekranu ramką `0xC0`.
- Audio jest wyciszone przez cały czas trzymania. Po puszczeniu klawisza jest
  **jeden skok** dekodera do pokazanej pozycji.
- FF do końca utworu przechodzi na następny utwór.

---

## 14. Zasilanie, BUS_ON i cykl życia sesji

[EMUL]
| Zdarzenie | Czas | Reakcja |
|---|:--:|---|
| zmiana BUS_ON | 12 ms | filtr drgań |
| BUS_ON = 0 | od razu | przerwanie nadawania, czyszczenie slotu; adres zapamiętany jako „z poprzedniej sesji” |
| BUS_ON = 0 trwa | 500 ms | stop audio, zapamiętanie pozycji (dłużej niż discovery radia) |
| BUS_ON = 0 trwa | 600 ms | zapis oczekującego crash logu na pendrive |
| BUS_ON = 0 trwa | 2 s | zapis stanu i **odcięcie własnego zasilania** (`PIN_POWER_LATCH` = LOW) |
| BUS_ON = 1 | — | podtrzymanie zasilania, wybudzenie zmieniarki, odpowiedź na ping pod adresem z NVS |

Stan Init → Idle: 800 ms po pierwszym pingu. Jeśli radio wybrało zmieniarkę,
a indeks USB jest gotowy, emulator od razu wznawia zapamiętany utwór.

---

## 15. Odporność na błędy i diagnostyka

### 15.1. Szybka ścieżka w przerwaniu
[EMUL] Odpowiedzi, bez których radio resetuje magistralę, wysyła samo
przerwanie, bez udziału zadania:
- ping `01 12` od `0x10` lub `0x14` → status,
- poll `18 10 01 15` → zgłoszenie z maską, gdy jest ramka do wysłania,
- grant `3X 10/14 01 13` → przygotowana ramka.

Dzięki temu odpowiedź zawsze zdąży przed slotem, także w trakcie zapisu do flash
(przerwanie jest w IRAM).

### 15.2. Odtwarzanie ramki z jednym zgubionym bitem
[POTW] W crash logach każda ucięta krótka ramka (`CUT`) to poprawna ramka radia
z **dokładnie jednym brakującym bitem** (zgubione zbocze zegara). Nie było ani
jednego bitu nadmiarowego. Przykład: `31 10 01 12 54 00` odebrane jako
`31 10 02 24 A8`.

[EMUL] Gdy master zaczyna slot, a odebrano tylko 47 bitów ramki 6-bajtowej,
przerwanie sprawdza, czy to jedna z ramek, na które musimy odpowiedzieć z
usuniętym jednym bitem:
- ping od `0x10`/`0x14`,
- grant od `0x10`/`0x14`,
- poll `18 10 01 15`.

Przy jednoznacznym dopasowaniu odtwarza ramkę i odpowiada w tym samym slocie
(wpis `FIX` w logu). Przy niejednoznacznym — milczy. Pozostałe ucięte ramki są
pomijane.

Sprawdzono offline:
- każda delecja z tych ramek dopasowuje się jednoznacznie,
- żadna z 59 innych ramek z logów z jednym zgubionym bitem nie podszywa się pod
  ramkę do nas (adresy `0x30–0x3F`),
- 0 fałszywych trafień na 200 tys. losowych ciągów.

### 15.3. Licznik sprzętowy zboczy (PCNT)
[EMUL] PCNT liczy zbocza CLOCK niezależnie od CPU. Przy każdej ramce `CUT`/`FIX`
crash log dostaje wpis:
```
EDGE PP II GG SS LL MM ; hex: zbocza wg PCNT, przyjęte przez przerwanie, glitch,
                       ;      spike, największe opóźnienie przerwania (µs),
                       ;      najkrótszy odstęp między zboczami w ramce (µs)
```
- `PP > II` — zbocze doszło do pinu, a ESP go nie przyjął.
- `PP == II` — zbocze nie dotarło czysto do pinu (sygnał lub sprzęt).

[POTW] Jazdy z 4–5.11.2026:
- wszystkie ucięte ramki miały `PP` równe **dokładnie** liczbie bitów ramki
  (48 dla 6 B, 88 dla 11 B), a przerwanie przyjęło o tyle mniej, ile było
  odrzuceń `glitch`/`spike`,
- po pomiarze opóźnienia okazało się, że przerwanie było w tych ramkach **na czas**
  (opóźnienie 7–9 µs).

Wniosek: wszystkie zbocza są prawdziwe, ale **CDX-M670 czasem taktuje dwa bity
w odstępie < 30 µs** (albo daje bardzo krótki stan wysoki). Dawny filtr 30 µs
wyrzucał wtedy prawdziwy bit. Od 5.11.2026 przy odbiorze filtr to 5 µs
(`BUS_CLOCK_GLITCH_RX_US`). 30 µs zostaje tylko na czas naszego nadawania
i Breaka (przesłuch z DATA). W `[STAT]` pole `short=` liczy bity przyjęte
dzięki tej zmianie.

### 15.3a. Opóźnienie przerwania (MCPWM capture)
[EMUL] Kanał 0 MCPWM0 zapisuje sprzętowo chwilę każdego zbocza CLOCK. Przerwanie
na wejściu wyzwala programowo kanał 1. Różnica to opóźnienie obsługi zbocza
(licznik taktowany APB, 80 MHz).
- W `[STAT]` pole `lat=MAX/N`: największe opóźnienie w µs i liczba zboczy
  obsłużonych później niż 30 µs.
- W osi czasu crash logu wpis `SLOW HH LL NN` (co najwyżej co 200 ms):
  opóźnienie ostatniego spóźnionego zbocza (µs, 16 bit) i liczba takich zboczy
  od poprzedniego wpisu.

Przy bicie ~125 µs filtr glitch (30 µs) gubi bit, gdy przerwanie spóźni się o
więcej niż ~95 µs.

### 15.3b. Unikanie alokacji na rdzeniu 1
[EMUL] Alokator sterty w IDF 4.4 (`malloc`/`free`) przeszukuje liniowo listę
wolnych bloków z wyłączonymi przerwaniami. Sterta jest pokawałkowana przez
~1000 obiektów `String` z indeksu utworów. Dlatego na rdzeniu 1 (przerwanie
zegara, zadanie magistrali, `loop()`):
- `Serial.printf` formatuje w stałym buforze 256 B (`WiFiLoggerClass::printf`)
  zamiast `Print::printf`, który dla linii > 63 znaków robi `malloc`,
- nazwy utworów do CD-TEXT kopiowane są bez tymczasowych `String`.

Po tej zmianie opóźnienie spadło do typowo 2–8 µs (maks. 45 µs). Liczba
zgubionych bitów się jednak nie zmieniła — ich przyczyną był filtr (§15.3).

### 15.4. Linia `[STAT]` (co 2 s)
| Pole | Znaczenie |
|---|---|
| `rx` | odebrane ramki |
| `ping`, `p15`, `claim`, `grant` | pingi do nas, polle `01 15`, nasze zgłoszenia, granty |
| `brk=a/b/c` | Breaki udane / bez pollu / bez fali idle |
| `late`, `abort` | odpowiedzi spóźnione / przerwane nadawania |
| `bad` | ramki `CUT` + `BAD` |
| `glitch`, `spike` | odrzucone zbocza (§2) |
| `cut` | niekompletne ramki (także pojedyncze fałszywe bity po Breaku) |
| `fix` | ramki odtworzone (§15.2) |
| `lost=a/b` | ucięte ramki: zbocze przegapione przez ESP / nie dotarło do pinu |
| `lat=m/n` | największe opóźnienie przerwania (µs) / liczba zboczy obsłużonych po > 30 µs |
| `short` | bity przyjęte w odstępie 5–30 µs od poprzedniego (dawniej gubione) |
| `ovf` | przepełnienie bufora ramek |

### 15.5. Crash log na pendrive (`/CrashLogs/crash_NNNNN.txt`)
Zawartość:
- ostatnie 128 zdarzeń magistrali: `RX`, `TX`, `CUT`, `BAD`, `FIX`, `EDGE`,
  `SLOW`, `BREAK`, `RESET`, `FLASH`/`FLEND`, `BUSON`/`BUSOF`,
- ostatnie ~8 KB logu tekstowego.

Powody zapisu:
| Powód | Warunek |
|---|---|
| `RADIO BUS RESET 18 10 01 00` | reset przy aktywnym adresie, > 15 s od startu i > 3 s od BUS_ON |
| `BUS_ON LOST (bez 87 OFF)` | zanik BUS_ON > 500 ms w trakcie grania ze zmieniarki, bez zapowiedzi wyłączenia `87` |
| `RADIO SILENT (BUS_ON=1)` | 5 s bez ramek przy BUS_ON = 1 |
| `ESP RESTART: …` / `ESP CRASH: …` | restart ESP po panic/WDT/brownout (log sprzed restartu z RAM `.noinit`) |

Zachowywane jest pierwsze niezapisane zdarzenie; maks. 10 plików z rotacją.

### 15.6. Zapisy do flash
[EMUL] Zapis NVS lub LittleFS zatrzymuje zadania na obu rdzeniach (przerwanie
działa dalej). Zapisy idą tylko w oknie **60–400 ms po naszym pingu**, czyli po
pingu procesora ekranu (+28 ms) i daleko przed następnym pingiem (~2,5 s). Przy
BUS_ON = 0 zapis jest dozwolony zawsze.

W 2 z 3 analizowanych crash logów zgubiony ping ekranu nakładał się na zapis
NVS, który startował 15 ms po pingu.

---

## 16. Nośnik USB i odtwarzanie

[EMUL]
- **Płyty** to foldery w katalogu głównym pendrive'a: `CD01` … `CD14`,
  opcjonalnie z nazwą po kropce — `CD04.Dokladka` → płyta 4 o nazwie
  „Dokladka” (to jest CD-TEXT płyty).
- **Utwory** to pliki `.mp3`, `.wav`, `.flac`, `.aac`, posortowane
  alfabetycznie i numerowane od 1, maks. 99 na płytę. Nazwa pliku to CD-TEXT
  utworu.
- **Indeks** zapisywany jest w LittleFS (`/unilink_index.dat`) razem z liczbą
  wolnych klastrów pendrive'a. Przy zmianie zawartości pendrive'a jest
  przebudowywany.
- **Wznowienie**: płyta, utwór i sekunda zapisywane są w NVS przy zmianie utworu
  i co 30 s. Po starcie dekoder czeka, aż zna bitrate pliku, i wtedy robi
  bezwzględny skok do zapisanej sekundy. Do tego czasu jest wyciszony, a radio
  widzi zapisany czas.

---

## 17. Poprawki względem poprzedniej wersji

| Było | Jest |
|---|---|
| okres zegara ~20 µs, wypełnienie 50% | **~125 µs na bit**, ~1 ms na bajt (pomiar) |
| „~3 ms przytrzymania DATA na początku pakietu” | nie występuje; ramki oddziela ~5 ms ciszy zegara, a po każdej ramce jest slot odpowiedzi |
| `0x90`: D1 = numer płyty (`F1` = płyta 1) | **D1 = numer utworu** (BCD F), D2 = minuty, D3 = sekundy, **D4 górny nibble = płyta** |
| `0xC0`: `F1` w D6 = numer płyty | **D6 = utwór, D7 = minuty, D8 = sekundy, D9 = płyta + flagi** |
| `0xD2`: D8 (`F1`) = numer płyty | **D8 = numer utworu**; numer płyty niesie dopiero ramka `0xDA` |
| `0x9C`: D4_hi = 8 → „brak nazwy” | D4 górny nibble = **numer płyty**, dolny `8` = płyta obecna |
| Seeking = `0x21` | CDX-M670 gasi wtedy licznik; emulator wysyła `0x00` |
| żądania tekstu tylko `84 D9`/`DD` | CDX-M670 używa głównie cyklu `D2`/`DA`/`D7` z flagami 8→B→A i żądania `84 D7` od `0x71` |
| nie opisano roli `0x14` | `0x14` (procesor ekranu) pinguje, wydaje granty i robi polle — brak odpowiedzi na jego ping = reset |
| nie opisano reguły resetu | ping bez odpowiedzi → ponowienie po ~600 ms → reset `18 10 01 00` |
| maska zgłoszenia nieopisana | maska = dolny nibble CMD2 ramki appoint, używana w `10 18 82 MM` |
| device info z dokumentacji (`8C 10 24 A8 17 A0`) | CDX-M670 akceptuje ramkę oryginału `8C D0 xx A8 1F A3` z P2 = `0B` |
| `0x95` = mapa bitów po kolei | mapa **przepleciona** (§11.1) |
| `0x97` D1 binarnie, D4 jako `F|nr` | D1–D3 w BCD, D4 = numer płyty w górnym nibblu |

---

## 18. Załącznik: zdekodowane przykłady

```
; ping mastera do 0x31 i odpowiedź (gra)
31 10 01 12 54 00           P1 = 31+10+01+12 = 54
10 31 00 00 41 00           status 00 = Playing

; ping procesora ekranu ~28 ms później
31 14 01 12 58 00  ->  14 31 00 00 45 00

; zgłoszenie i grant
18 10 01 15 3E 00
10 18 82 02 AC 00 00 00 00 AC 00     maska 02
31 10 01 13 55 00
70 31 90 30 61 13 F0 01 4A AF 00     TR13, 0:01, CD4, flaga A
                                     P2 = 61+13+F0+01+4A = AF

; pełny status po zmianie utworu (CD4, TR13, flaga 8)
70 31 C0 00 61 00 00 00 00 30 13 F0 00 48 DC 00

; CD-TEXT: „Automat - Droid” (12 znaków: „Automa” + „t - Dr”), płyta „Dokladka”
70 31 D2 41 B4 75 74 6F 6D 61 02 00 12 10 FE 00    "Automa", ciąg dalszy, TR12
70 31 D2 74 E7 20 2D 20 44 72 00 01 12 10 2D 00    "t - Dr", ostatni
70 31 DA 44 BF 6F 6B 6C 61 64 00 01 04 10 DF 00    "Dokladka" -> "Doklad", płyta 4
70 31 D7 00 78 00 00 00 00 00 02 00 00 1C 96 00    koniec bloku

; reset magistrali przez radio
18 10 01 00 29 00

; ping z jednym zgubionym bitem (odebrany) i po odtworzeniu
31 10 02 24 A8 (+7 bitów)   ->  31 10 01 12 54 00
```
