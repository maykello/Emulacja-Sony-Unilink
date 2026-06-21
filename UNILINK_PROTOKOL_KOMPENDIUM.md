# Sony Unilink — kompendium protokołu magistrali

Dokumentacja techniczna pod budowę **emulatora zmieniarki CD** dla radia Sony.

Opracowano na podstawie:
1. **Pasywnego sniffu** komunikacji radia **Sony CDX-M670** ze zmieniarką
   **CDX-805** (plik `unilink_log_zmieniarka_CDX-M670_20260605_165726.txt`,
   sniffer „Sony UniLink SPY v2”, ESP32-S3),
2. **Dokumentacji Mictronics** „Inside Sony Unilink” oraz tablicy komend
   (`command.htm`) — najpełniejszego publicznego opisu protokołu (wynik
   inżynierii wstecznej społeczności unilink),
3. **Działających implementacji emulatorów** open-source:
   - Becker/AVR Unilink (Michael Wolf) — `unilink.c`,
   - STM32 „Alfa-166 Unilink CD emulator” (deividAlfa) — nowszy, z CD-TEXT i USB.

Wszystkie pobrane materiały leżą w podkatalogu `./zrodla/` (HTML dokumentacji
i kod źródłowy emulatorów).

> **Znaczniki wiarygodności:**
> - ✅ **Potwierdzone** — zgodne między logiem, dokumentacją Mictronics i kodem
>   emulatorów (zweryfikowane też arytmetycznie).
> - 🔶 **Wywnioskowane** — z obserwacji; prawdopodobne, lecz niegwarantowane dla
>   każdego modelu radia.
> - ℹ️ **Tło / wariant** — informacja zależna od modelu urządzenia.
>
> Treści ze źródeł zewnętrznych sparafrazowano dla zgodności z licencjami.

---

## Spis treści
1. [Warstwa fizyczna — trzy linie i zegar](#1-warstwa-fizyczna--trzy-linie-i-zegar)
2. [Stany magistrali, wybudzanie i slave-break](#2-stany-magistrali-wybudzanie-i-slave-break)
3. [Struktura ramki (RAD/TAD/CMD/parzystości)](#3-struktura-ramki-radtadcmdparzystości)
4. [Dwie sumy kontrolne — dokładnie](#4-dwie-sumy-kontrolne--dokładnie)
5. [Adresowanie: grupy i DYNAMICZNY przydział ID](#5-adresowanie-grupy-i-dynamiczny-przydział-id)
6. [Inicjalizacja i przydział adresu (Initial Link / Appoint)](#6-inicjalizacja-i-przydział-adresu-initial-link--appoint)
7. [Cykl pracy: time-poll, request-poll, status](#7-cykl-pracy-time-poll-request-poll-status)
8. [Tablica komend](#8-tablica-komend)
9. [Przewijanie i sterowanie odtwarzaniem (szczegółowo)](#9-przewijanie-i-sterowanie-odtwarzaniem-szczegółowo)
10. [CD-TEXT i nazwy płyt/utworów (szczegółowo)](#10-cd-text-i-nazwy-płytutworów-szczegółowo)
11. [Status odtwarzania i pozycja (0x90 / 0xC0)](#11-status-odtwarzania-i-pozycja-0x90--0xc0)
12. [Wytyczne dla emulatora zmieniarki](#12-wytyczne-dla-emulatora-zmieniarki)
13. [Załącznik A: format logu snifera](#załącznik-a-format-logu-snifera)
14. [Załącznik B: przykłady z pełnym dekodowaniem](#załącznik-b-przykłady-z-pełnym-dekodowaniem)
15. [Źródła](#źródła)

---

## 1. Warstwa fizyczna — trzy linie i zegar

✅ Unilink to **synchroniczna magistrala z zegarem od mastera** (a nie czysty
single-wire PWM jak pokrewny S-Link). Według dokumentacji Mictronics i zgodnie
z obserwowanym sniffem:

- Sygnały **TTL: 0 V = LOW, 5 V = HIGH** (sniffer ESP32-S3 wymaga konwersji
  5 V → 3,3 V).
- Magistrala ma **trzy linie** sygnałowe (plus masa i zasilanie):
  | Linia | Rola |
  |---|---|
  | **BUS ON** | zasilanie/aktywacja magistrali (master ją włącza) |
  | **CLOCK** | zegar taktujący, generuje **master** dla wszystkich slave'ów |
  | **DATA** | dwukierunkowa linia danych (open-collector, wired-OR) |

- **Taktowanie bitu:** okres zegara ~**20 µs**, wypełnienie 50%. Dana na linii
  DATA zmienia się w połowie wysokiego stanu zegara.
- **Tempo bajtów:** zegar „bajtowy” ~1 ms → **jeden bajt na ~1 ms**.
- Na początku każdego nowego pakietu jest **~3 ms** przytrzymania DATA w stanie
  niskim (znacznik startu pakietu).

> W logu snifera widać to pośrednio: odstępy `dt≈6000 µs` między kolejnymi
> ramkami transakcji oraz `dt≈0,5–0,6 s` między rundami odpytywania w spoczynku.
> Sniffer mierzy `minBitGap` (raportuje `1us`).

---

## 2. Stany magistrali, wybudzanie i slave-break

✅ Magistrala jest zawsze w jednym z trzech stanów:
- **off** — `BUS ON = 0`, linie CLOCK/DATA bez znaczenia (tryb uśpienia).
- **idle** — `BUS ON = 1`, `CLOCK = 0`, a DATA generuje **falę prostokątną**:
  ~8 ms low, potem ~8 ms high (utrzymywaną przez mastera).
- **active** — `BUS ON = 1`, trwa transmisja taktowana zegarem.

### 2.1. Wybudzanie (Wake Up)
🔶/✅ Slave może **obudzić** mastera, wymuszając stan **wysoki** na linii DATA.
Master odpowiada **włączeniem linii BUS ON**. W logu początek to pulsowanie
`BUS=1/0`, aż magistrala ustala `bus=1` i rusza komunikacja.

### 2.2. Slave-break (slave chce nadać)
✅ Gdy urządzenie podrzędne ma dane do wysłania, sygnalizuje to **slave-break**
w trakcie stanu idle (kod emulatorów potwierdza dokładne czasy):
1. odczekaj, aż DATA jest **low ≥ ~7–8 ms** (magistrala w idle),
2. odczekaj ~**2 ms** na stan **high** DATA (nadal idle),
3. **wymuś DATA low na ~3 ms**,
4. zwolnij DATA do high na ostatnie ~3 ms.

Cały slave-break mieści się w jednym ~8 ms wysokim oknie idle. Po nim master
wie, że ktoś chce mówić, i uruchamia **request-poll** (patrz §7), pytając „kto
chce mówić” (`0x01 0x15`), a następnie daje pozwolenie (`0x01 0x13`).

---

## 3. Struktura ramki (RAD/TAD/CMD/parzystości)

✅ Każdy „word” (ramka) ma jeden z **trzech rozmiarów**, a rozmiar wyznacza
**wartość CMD1** (3. bajt). Reguła z kodu emulatorów (potwierdzona w logu):

| CMD1 | Rozmiar ramki | Nazwa |
|---|---|---|
| `CMD1 < 0x80` | **6 bajtów** | short word |
| `0x80 ≤ CMD1 < 0xC0` | **11 bajtów** | middle word |
| `CMD1 ≥ 0xC0` | **16 bajtów** | long word |

(Rozmiary liczone wraz z końcowym bajtem `0`.)

### 3.1. Układy ramek
```
short  (6B):  RAD TAD CMD1 CMD2 Parity1 0
middle (11B): RAD TAD CMD1 CMD2 Parity1 D1 D2 D3 D4 Parity2 0
long   (16B): RAD TAD CMD1 CMD2 Parity1 D1 D2 D3 D4 D2_1 D2_2 D2_3 D2_4 D2_5 Parity2 0
```

| Pole | Znaczenie |
|---|---|
| **RAD** | *Receiver address* — adres odbiorcy (w mojej wcześniejszej notacji „DEST”). |
| **TAD** | *Transmitter address* — adres nadawcy („SRC”). |
| **CMD1** | główny bajt komendy (zarazem wyznacza długość ramki). |
| **CMD2** | podkomenda **albo** pierwszy bajt danych (zależnie od CMD1). |
| **Parity1** | suma kontrolna nagłówka (patrz §4). |
| **D1–D4** | cztery bajty danych (middle/long). |
| **D2_1–D2_5** | kolejne pięć bajtów danych (tylko long). |
| **Parity2** | druga suma kontrolna (patrz §4). |
| **0** | bajt zerowy kończący **każdy** word. |

> To dokładnie pokrywa się z moją wcześniejszą, empiryczną analizą logu
> (`DEST SRC CMD D0 [HDR-CHK …] CHK 00`) — Mictronics nadaje tym polom oficjalne
> nazwy. „Parity” to mylące określenie: w istocie to **sumy kontrolne**.

---

## 4. Dwie sumy kontrolne — dokładnie

✅ Zweryfikowane arytmetycznie na ramkach z logu oraz zgodne z kodem emulatorów:

- **Parity1** = `(RAD + TAD + CMD1 + CMD2) mod 256`
  — suma czterech bajtów nagłówka.
- **Parity2** = `(suma WSZYSTKICH bajtów danych przed nią, łącznie z tymi, które
  już weszły do Parity1, ale BEZ samego Parity1) mod 256`
  — czyli `(RAD+TAD+CMD1+CMD2 + D1+…+D2_5) mod 256`.

Równoważnie (wygodne w implementacji):
```
Parity1 = (RAD + TAD + CMD1 + CMD2) & 0xFF
Parity2 = (Parity1 + D1 + D2 + ... + ostatni_bajt_danych) & 0xFF
```
W **short word** nie ma Parity2 — Parity1 jest jedyną sumą (i pokrywa się z nią).

### 4.1. Weryfikacja na realnych ramkach (z logu)
```
3B 10 02 11 5E 0                  ; short
  Parity1 = 3B+10+02+11 = 5E  ✔

18 70 8C 00 |14| 80 05 0A 01 A4 0  ; middle (CMD1=8C ≥0x80)
  Parity1(=byte4) = 18+70+8C+00 = 14  ✔
  Parity2(=A4)    = 14+80+05+0A+01 = A4  ✔

70 31 D2 4B |BE| 61 70 69 74 61 02 00 F1 10 D0 0  ; long (CMD1=D2 ≥0xC0)
  Parity1(=BE) = 70+31+D2+4B = BE  ✔
  Parity2(=D0) = BE+61+70+69+74+61+02+00+F1+10 = D0  ✔
```

### 4.2. Pseudokod (nadawanie)
```c
uint8_t p1 = (RAD + TAD + CMD1 + CMD2) & 0xFF;
uint8_t p2 = p1;
for (i = 0; i < n_data; i++) p2 = (p2 + data[i]) & 0xFF;   // D1..D2_5
// short : RAD TAD CMD1 CMD2 p1 0
// middle: RAD TAD CMD1 CMD2 p1 D1 D2 D3 D4 p2 0
// long  : RAD TAD CMD1 CMD2 p1 D1..D2_5 p2 0
```
Bajt końcowy `0` **nie** wchodzi do żadnej sumy.
---

## 5. Adresowanie: grupy i DYNAMICZNY przydział ID

To jedna z najważniejszych poprawek względem pierwszej wersji dokumentu.

✅ **Adres = dwa nibble:**
- **górny nibble = GRUPA urządzenia** (typ, stały, wynika z rodzaju sprzętu),
- **dolny nibble = ID urządzenia w grupie** — **przydzielany DYNAMICZNIE przez
  mastera** podczas inicjalizacji (Initial Link).

> **DLATEGO emulator NIE ma „na sztywno” adresu 0x31.** Zmieniarka należy do
> **grupy `0x3` (CD players/changers)**, ale konkretny adres (`0x30`, `0x31`,
> `0x32`, …) **nadaje radio** przy starcie. W naszym logu akurat wypadło `0x31`
> (grupa 3, ID 1), ale przy innym zestawie urządzeń może to być inny ID. Emulator
> musi **zgłosić swoją grupę** i **przyjąć ID** przydzielony przez mastera, a
> potem używać go jako swojego adresu. (Kod emulatorów startuje z `ownaddr=0x30`
> = „grupa CD, brak ID” i nadpisuje go przydzielonym adresem — patrz §6.)

✅ Jeśli w polu **RAD** dolny nibble = `0`, to **broadcast do całej grupy**
(np. `0x30` = do wszystkich urządzeń CD).

### 5.1. Grupy urządzeń (górny nibble)
| Grupa | Typ urządzenia |
|:--:|---|
| `0x3` | **Odtwarzacze / zmieniarki CD** ← nasza grupa |
| `0x5` | Tunery |
| `0x6` | Magnetofony kasetowe |
| `0x7` | Wyświetlacz (Display) |
| `0x8` | Multiplekser audio/bus (np. XA-C30) |
| `0xC` | Zegar |
| `0xD` | Zmieniarki MD |

### 5.2. Adresy specjalne i pozostałe
| Adres | Rola |
|:--:|---|
| **`0x10`** | **Master** (radio / głowica) — odpytuje, odbiera ACK i odpowiedzi |
| **`0x18`** | **Broadcast** (do wszystkich) — stąd częste `18 10 01 …` = rozgłoszeniowy poll |
| **`0x70`** | **Broadcast do grupy Display** (id 0) — tu slave'y „zrzucają” dane do pokazania na ekranie |
| `0x11` | Klawiatura (Keypad) |
| `0x21` | Klawiatura (sterowanie) |
| `0x91` | DSP (głośność, barwa itp.) |

### 5.3. Korekta wcześniejszych wniosków (z analizy samego logu)
Surowa analiza logu sugerowała role, które dokumentacja Mictronics doprecyzowuje:
| Adres w logu | Pierwotny wniosek | **Poprawnie (Mictronics)** |
|:--:|---|---|
| `0x18` | „panel przedni” | **Broadcast** (adres rozgłoszeniowy) |
| `0x70`/`0x77` | „punkt danych głowicy” | **Broadcast/element grupy Display `0x7`** |
| `0x31` | „zmieniarka” | ✅ Zmieniarka: **grupa 3, ID 1 (przydzielone)** |
| `0x3B` | „kanał obecności” | inne urządzenie **grupy CD** (ID B) |
| `0x71` | „kanał zmieniarki” | urządzenie **grupy Display** (ID 1), **nie** zmieniarka |
| `0x51` | „tuner” | ✅ Tuner (grupa 5) |
| `0x91` | „DSP” | ✅ DSP |
| `0xDB` | „adres tymczasowy” | **zmieniarka MD** (grupa D, ID B) |

---

## 6. Inicjalizacja i przydział adresu (Initial Link / Appoint)

✅ Procedura wykrywania i nadawania adresów (Mictronics + kod emulatorów):

1. **Wake up** — magistrala się budzi (`BUS ON = 1`).
2. **„Anyone?”** — master rozsyła `CMD1=0x01, CMD2=0x02` (*anyone / initial link*).
3. **Zgłoszenie typu** — slave bez ID (zmieniarka startuje z `ownaddr=0x30`)
   odpowiada **stringiem informacyjnym o urządzeniu** (`CMD1=0x8C`) — mówi
   masterowi, do jakiej grupy należy i jakie ma możliwości.
4. **Przydział (Appoint)** — master wysyła pakiet do **mojej grupy** z konkretnym
   adresem (np. RAD=`0x31`). Slave **zapamiętuje nowy adres** i potwierdza,
   ponownie wysyłając swój string informacyjny.
5. **„Appoint end”** — master kończy `CMD1=0x01, CMD2=0x04`.
6. Master sprawdza `Anyone special (0x03)` / `Link On`, czy są kolejne urządzenia.

### 6.1. String informacyjny o urządzeniu (`0x8C`) ✅
Z dokumentacji Mictronics (przykłady „device info”):
```
CMD1 CMD2  D1    D2    D3    D4
0x8C 0xA0  0x06  0xA8  0x25  0xA0   ; zmieniarka CDX-605 / CDX-91
0x8C 0x89  0x15  0xAC  0x17  0xA0   ; zmieniarka CDX-71
0x8C 0x10  0x24  0xA8  0x17  0xA0   ; przykład „device info” (użyty w emulatorze AVR)
0x8C 0x00  0x24  0x2C  0x22  0xA0   ; napęd MD zewnętrzny
```
Znaczenie bajtów (🔶 wg Mictronics):
- **D2:** `0001xxxx` = urządzenie wewnętrzne (1 slot), `0010xxxx` = zewnętrzne.
- **D3:** `0001xxxx` = „Custom file”, `0010xxxx` = brak Custom file.
- **D4:** `1111 0000` — górne bity = **liczba slotów na płyty**.

W emulatorze AVR string to:
```c
#define CD_DEVICE_MSG {0x10, ownaddr, 0x8C, 0x10, 0x24, 0xA8, 0x17, 0xA0}
```
(wysyłany do mastera `0x10`).

### 6.2. Obsługa „Anyone” i „Appoint” w emulatorze (wzorzec) ✅
```
JEŻELI RAD == 0x18 (broadcast):
    JEŻELI CMD1==0x01 i CMD2==0x02 (Anyone) i nie mam ID:
        wyślij CD_DEVICE_MSG     // zgłoś grupę CD
    JEŻELI CMD1==0x01 i CMD2==0x00 (Bus reset):
        ownaddr = 0x30           // skasuj przydzielony ID

JEŻELI (ownaddr == 0x30) i CMD1==0x02 i (RAD & 0xF0)==0x30 (Appoint do mojej grupy):
    ownaddr = RAD                // ZAPAMIĘTAJ przydzielony adres (np. 0x31)
    wyślij CD_DEVICE_MSG         // potwierdź
```

---

## 7. Cykl pracy: time-poll, request-poll, status

✅ Po inicjalizacji master cyklicznie odpytuje urządzenia (`CMD1=0x01`).
Podkomendy (CMD2) — z dokumentacji i nagłówka emulatora:

| CMD1 | CMD2 | Znaczenie |
|:--:|:--:|---|
| `0x01` | `0x00` | Re-initialize bus (reset) |
| `0x01` | `0x02` | **Anyone?** (initial link) |
| `0x01` | `0x03` | Anyone special? |
| `0x01` | `0x04` | Appoint end |
| `0x01` | `0x11` | Time poll end |
| `0x01` | `0x12` | **Time poll** — slave odpowiada **statusem** (PONG) |
| `0x01` | `0x13` | **Request time poll** — pozwolenie na nadanie danych |
| `0x01` | `0x15` | „Kto chce mówić?” (po slave-break) |

### 7.1. Odpowiedź na time-poll (status, PONG) ✅
Na `0x01 0x12` slave odsyła **short word** `CMD1=0x00, CMD2=status`:
```c
#define STATUS_MSG {0x10, ownaddr, 0x00, unilink_status}
```
Wartości **status** (CMD2 przy `CMD1=0x00`, „Status from slave”):
| Status | Znaczenie |
|:--:|---|
| `0x00` | Playing (odtwarzanie) |
| `0x01` | Playing (wariant) |
| `0x20` | Changed CD (zmieniono płytę) |
| `0x21` | **Seeking (przewijanie)** |
| `0x40` | Changing CD (trwa zmiana płyty) |
| `0x80` | Idle (bezczynność) |
| `0xC0` | Ejecting |
| `0xF0` | Remote Joystick key |
| `0xFA` | Drugi poziom klawisza SEEK |

🔶 Typowa „maszynka” stanów w emulatorze po kolejnych time-pollach:
`0x40 (changing) → 0x20 (changed) → 0x21 (seeking) → 0x00 (playing)`.

### 7.2. Pozwolenie na nadanie (`0x01 0x13`) ✅
Gdy slave wcześniej zrobił **slave-break** (miał coś do powiedzenia), master daje
mu głos przez `0x01 0x13`. Slave wysyła wtedy **zakolejkowaną** ramkę: status
płyty (`0xC0`), info magazynka (`0x8E/0x95`), nazwę (`0xC9/0xCD/0xD2`), czas
(`0x90`) itd.
---

## 8. Tablica komend

✅ Zestawienie najważniejszych komend (`CMD1`) wg dokumentacji Mictronics
(`command.htm`), potwierdzonych w logu i kodzie emulatorów. Kolumna „Typ”:
`m` = od mastera, `s` = do/od slave (sterujące), `l` = long word z danymi.

### 8.1. Komendy sterujące (master → zmieniarka)
| CMD1 | CMD2 | Funkcja | Uwagi |
|:--:|:--:|---|---|
| `0x01` | (pkt 7) | System/poll | „0x01…” to komendy systemowe — błąd CRC = pełna reinicjalizacja |
| `0x02` | `0x01` | Konfiguracja: przydziel ID | Appoint |
| `0x08` | `0x00` | **Key off (Cancel)** | wysyłane po **puszczeniu** klawisza (kończy FF/REW) |
| `0x20` | — | **PLAY** | start odtwarzania |
| `0x24` | — | **Fast Forward (przewijanie w przód)** | start; **koniec** = broadcast `0x08` |
| `0x25` | — | **Fast Reverse (przewijanie w tył)** | start; **koniec** = broadcast `0x08` |
| `0x26` | — | **Next Track** (następny utwór) | |
| `0x27` | — | **Previous Track** (poprzedni utwór) | |
| `0x28` | — | **Next CD** (następna płyta) | |
| `0x29` | — | **Previous CD** (poprzednia płyta) | |
| `0x34` | — | Toggle Repeat (CD/MD) | przełącza tryby repeat |
| `0x35` | — | Toggle Shuffle | |
| `0x36` | — | Toggle Intro | |
| `0x37` | — | Toggle Bank | |
| `0x41` | `0x00/40/50` | Edycja nazwy płyty (kursor / znak +/−) | tryb edycji nazwy |
| `0x84` | `0xD9/0xDD/0x95/0x97` | **Żądanie pól tekstu / info** | patrz §10 (CD-TEXT) |
| `0xB0` | disc | **Bezpośredni wybór płyty**; D1 = utwór | „Set position” |

### 8.2. Komendy/odpowiedzi od zmieniarki (→ master `0x10` lub display `0x70`)
| CMD1 | Funkcja | Kluczowe pola |
|:--:|---|---|
| `0x00` | **Status** (odpowiedź na time-poll) | CMD2 = status (§7.1) |
| `0x8C` | Device info / „anyone respond” | identyfikacja urządzenia (§6.1) |
| `0x8E` | Info magazynka/kasety | `0x40` slot pusty, `0x80` włożono magazynek, `0xC0` brak magazynka |
| `0x90` | **Pozycja odtwarzania** | D1=utwór, D2=min, D3=sek, D4_hi=płyta (§11) |
| `0x91` | Bieżący czas / znaki nawigacji | |
| `0x94` | Tryby Repeat/Shuffle/Intro/Bank (ikony) | bitowe pola D1–D3 |
| `0x95` | Mapa obecności płyt | bit = płyta obecna (po skanie magazynka) |
| `0x97` | Info płyty | D1=liczba utworów, D2=min, D3=sek, D4_hi=płyta |
| `0x98` | Koniec Intro play | |
| `0x9C` | Zmiana płyty → RAD `0x90` | D4_lo=nr płyty, D4_hi: 8=brak nazwy, C=nazwa dostępna |
| `0xC0` | **Status play / pole tunera** (long) | dysk, min, sek, sterowanie ekranem (§11) |
| `0xC1` | Info PTY stacji (long, tuner) | |
| `0xC5` | **Identyfikator płyty (disc ID)** — po skanie/zmianie magazynka | unikatowy nr płyty |
| `0xC9` | **Nazwa utworu** (pole 0–1) | ASCII (§10) |
| `0xCD` | **Nazwa płyty** (pole 0–1) | ASCII (§10) |
| `0xCE`/`0xCF` | Nazwa płyty (Custom file; CF = miga = aktualnie grana) | po klawiszu LIST |
| `0xD2` | **CD-TEXT: nazwa utworu w trybie CD** | ASCII (§10) — widziane w naszym logu |
| `0xD5` | Identyfikator płyty (przy zmianie w trakcie grania) | jak `0xC5` |
| `0xD9` | **Nazwa utworu** (pola 2–5) | ASCII (§10) |
| `0xDD` | **Nazwa płyty** (pola 2–5) | ASCII (§10) |

### 8.3. Klawisze pilota/joysticka (slave → master, dla kontekstu)
`0x10` OFF/SOURCE, `0x12` DISP/LIST, `0x70` VOL−/VOL+, `0x71` MUTE, `0x74` SOUND,
`0x7E` PREV/NEXT DISC i **SEEK−/SEEK+**, `0x3C` bezpośrednie klawisze płyt 1–10.
(Wartości CMD2: `…0` = wciśnięty, `…A` = puszczony.)

---

## 9. Przewijanie i sterowanie odtwarzaniem (szczegółowo)

To była jedna z dwóch rzeczy, o które prosiłeś. Poniżej kompletny obraz.

### 9.1. Model „press / hold / release”
✅ Klawisze ciągłe (przewijanie) działają w schemacie **start → trzymanie →
koniec**:
- **wciśnięcie** SEEK+ / SEEK− → master wysyła do zmieniarki `CMD1=0x24`
  (Fast Forward) lub `CMD1=0x25` (Fast Reverse),
- **puszczenie** klawisza → master wysyła **broadcast `CMD1=0x08, CMD2=0x00`**
  (Key off / Cancel) — to **sygnał zakończenia** przewijania.

Innymi słowy: przewijanie trwa od `0x24`/`0x25` aż do `0x08`. (Potwierdzone w
tablicy Mictronics: „start fast forwarding, end with broadcast cmd 0x08”.)

### 9.2. Co robi zmieniarka (emulator)
✅ Na podstawie kodu emulatorów:
- po `0x24` → zacznij przewijać w przód (w emulatorze MP3: skok pozycji do
  przodu / szybkie odtwarzanie),
- po `0x25` → przewijaj w tył,
- ustaw **status = `0x21` (Seeking)** zwracany przy time-pollach,
- po broadcast `0x08` → **zatrzymaj** przewijanie, wróć do `status = 0x00`
  (Playing), zaktualizuj i wyślij nową pozycję (`0x90` / `0xC0`).

### 9.3. Sygnalizacja „seeking” na wyświetlaczu
🔶 W trybie pokazywania na ekranie master/zmieniarka używają pola `0xC0`/`0xC1`
do wyświetlenia stanu przewijania (np. „--.--” lub migający „LOAD”). Emulator
STM32 wykrywa przewijanie m.in. po `0xC1` z `CMD2=0x2D` (`'-'`).

### 9.4. Pozostałe sterowanie (mapowanie 1:1)
| Klawisz radia | Komenda do zmieniarki | Reakcja emulatora |
|---|:--:|---|
| ► PLAY | `0x20` | start grania, wyślij pozycję (`0xC0`) |
| ≫ SEEK+ (hold) | `0x24` … `0x08` | przewijaj w przód, status `0x21` |
| ≪ SEEK− (hold) | `0x25` … `0x08` | przewijaj w tył, status `0x21` |
| ⏭ następny utwór | `0x26` | track++ |
| ⏮ poprzedni utwór | `0x27` | track−− |
| DISC+ | `0x28` | disc++ , status `0x40`→`0x20` |
| DISC− | `0x29` | disc−− |
| wybór płyty 1–10 | `0xB0` (CMD2=disc, D1=track) | ustaw płytę/utwór, status `0x40` |
| REPEAT/SHUF/INTRO | `0x34/0x35/0x36` | przełącz tryb, wyślij `0x94` (ikony) |

> **Uwaga o adresach komend:** w naszym logu komendy transportu przychodziły od
> nadawcy **TAD=`0x11` (klawiatura)**, np. `31 11 28 00 6A` = „Next CD” do
> zmieniarki `0x31`. Emulator powinien reagować na daną komendę **niezależnie od
> tego, czy TAD to `0x10` (master) czy `0x11` (keypad)** — liczy się RAD = mój
> adres i CMD1.
---

## 10. CD-TEXT i nazwy płyt/utworów (szczegółowo)

Druga rzecz, o którą prosiłeś. Mechanizm jest **„żądanie pola → odpowiedź z
fragmentem”**, zorganizowany w **pola po znaki**.

### 10.1. Kto inicjuje
✅ To **wyświetlacz/master żąda** kolejnych pól tekstu komendą **`CMD1=0x84`**:
| `0x84` CMD2 | Co żądane | Parametr |
|:--:|---|---|
| `0xD9` | **nazwa utworu** | następny bajt (D1) = **numer pola** (0–5) |
| `0xDD` | **nazwa płyty** | D1 = numer pola (0–5) |
| `0x95` | info magazynka | — |
| `0x97` | czas całk. i liczba utworów płyty | — |

Zmieniarka kolejkuje odpowiedź i wysyła ją po najbliższym `0x01 0x13` (po
slave-break).

### 10.2. Klasyczne nazwy: 8 znaków na pole (`0xC9/0xD9`, `0xCD/0xDD`)
✅ Z kodu emulatora STM32 — nazwy dzielone są na **pola po 8 znaków**
(pola 0–5 ⇒ do 48 znaków). Wybór komendy zależy od numeru pola:

| Zawartość | Pole 0–1 | Pola 2–5 |
|---|:--:|:--:|
| **Nazwa utworu** | `CMD1=0xC9` | `CMD1=0xD9` |
| **Nazwa płyty** | `CMD1=0xCD` | `CMD1=0xDD` |

Budowa ramki nazwy (long word, 8 znaków ASCII):
```
{ 0x70(display), ownaddr, CMD1, c0, c1, c2, c3, c4, c5, c6, c7, 0x00, pole }
   RAD          TAD       CMD1  CMD2 D1 D2 D3 D4 D2_1 D2_2 D2_3 D2_4 D2_5
```
— czyli **8 znaków** trafia w `CMD2, D1, D2, D3, D4, D2_1, D2_2, D2_3`, a
`D2_4=0x00`, `D2_5 = numer pola` (offset = pole × 8). Logika pól w emulatorze:
pole 0 (`0x0E`) → po wysłaniu ustaw pole 1 (`0x1E`) i wyślij ponownie; pola 2–5
liczone wprost; numer > 5 kończy przesyłanie (`0x0E`).

### 10.3. CD-TEXT `0xD2` (wariant z naszego logu) ✅
W sniffie CDX-M670 ↔ CDX-805 zmieniarka używała komendy **`0xD2`** („CD-Text
track name in CD mode”). Tu **pierwszy znak siedzi w `CMD2`**, a ramka niesie
**6 znaków**:
```
70 31 D2 4B BE 61 70 69 74 61 02 00 F1 10 D0 0
RAD=70 TAD=31 CMD1=D2
 CMD2=4B='K'
 Parity1=BE
 D1=61='a' D2=70='p' D3=69='i' D4=74='t'
 D2_1=61='a'        -> znaki: "Kapita"
 D2_2=02  (licznik/kontynuacja)
 D2_3=00
 D2_4=F1  (numer płyty: F1 = płyta 1)
 D2_5=10  (typ pola tekstu)
 Parity2=D0
```
🔶 Pola pomocnicze wg Mictronics dla `0xD2`: `D2_2` w polu 0 ≈ `0x04`,
`D2_3` = licznik pól, `D2_4` ≈ `0xF6` (u nas `F1…F4` = numer płyty). Wartości
bywają różne między modelami — istotne jest, że **znaki zaczynają się od `CMD2`**.

### 10.4. Tekst odczytany z naszego logu (poprawnie) ✅
Po prawidłowym dekodowaniu (pierwszy znak = `CMD2`):
| Płyta (D2_4) | Fragmenty (kolejne `0xD2`) | Złożony tekst |
|:--:|---|---|
| `F1` | `Kapita` + `nskie t…` | **„Kapita[ń]skie t…”** |
| `F2` | `Do zak` + `ochania` | **„Do zakochania”** |
| `F3` | `Mary A` + `nn…` | **„Mary Ann…”** (🔶) |
| `F4` | `Starus` + `zek Świ…` | **„Staruszek Świ[ęty]…”** (🔶) |

(W pierwszej wersji dokumentu te fragmenty były błędnie zdekodowane, bo `CMD2`
potraktowano jako offset, a nie jako pierwszy znak — teraz poprawione.)

### 10.5. Jak emulator ma podawać własny CD-TEXT
✅ Wzorzec działania:
1. Trzymaj nazwy jako bufory znaków (np. nazwa utworu = nazwa pliku MP3).
2. Po starcie/zmianie płyty zrób **slave-break**, by zgłosić dostępność tekstu;
   w `0x9C`/`0xC5` ustaw flagę „nazwa dostępna” (D4_hi = `C`).
3. Na `0x84 0xD9` (żądanie pola nazwy utworu, numer pola w D1) odeślij pole:
   - pole 0–1 → `0xC9`, pola 2–5 → `0xD9` (8 znaków, offset = pole×8),
   - albo użyj wariantu `0xD2` (6 znaków od `CMD2`), jeśli celujesz w zachowanie
     jak CDX-805.
4. Analogicznie `0x84 0xDD` → nazwa płyty (`0xCD`/`0xDD`).
5. Gdy numer pola > 5 lub tekst się skończył — przestań wysyłać kolejne pola.

> **Kodowanie znaków:** ASCII 7-bit (w logu m.in. polskie wyrazy zapisane bez
> diakrytyków lub z podmianą — `Kapitanskie`, `Swiety`). Dla bezpieczeństwa
> ogranicz wysyłany tekst do ASCII drukowalnego (0x20–0x7E).

---

## 11. Status odtwarzania i pozycja (0x90 / 0xC0)

### 11.1. JAK DZIAŁA ODLICZANIE CZASU UTWORU ✅
**Najważniejszy wniosek: zegar prowadzi ZMIENIARKA, a nie radio.** Radio tylko
wyświetla liczbę, którą dostaje. To zmieniarka utrzymuje upływający czas
bieżącego utworu i co sekundę wypycha nową wartość na magistralę.

**Pełna sekwencja (zweryfikowana w logu, znaczniki czasu hosta):**
```
18 10 01 15 3E 0                  ; master: "kto chce mówić?" (broadcast 0x15)
31 10 01 13 55 0                  ; master daje ZMIENIARCE głos (request poll 0x13)
70 31 90 30 61 F1 F0 03 8B D0 0   ; zmieniarka -> wyświetlacz 0x70: czas = 03 s
... (~1 s później) ...
70 31 90 30 61 F1 F0 04 8A D0 0   ; czas = 04 s
70 31 90 30 61 F1 F0 05 8A D1 0   ; czas = 05 s
70 31 90 30 61 F1 F0 06 8A D2 0   ; czas = 06 s
```
Pomiar odstępów między tymi ramkami: ~0,97–1,02 s → **inkrement o 1 sekundę**.

**Krok po kroku, jak czas trafia na ekran:**
1. Zmieniarka odlicza czas własnym zegarem 1 Hz (ma stały takt ~1 s).
2. Gdy ma nową wartość, sygnalizuje chęć nadania przez **slave-break** (§2.2).
3. Master pyta rozgłoszeniowo `0x01 0x15` („kto chce mówić?”).
4. Master udziela głosu zmieniarce: `RAD=mój_adres, 0x01 0x13`.
5. Zmieniarka wysyła **middle word `0x90`** na adres wyświetlacza `0x70`
   z nową liczbą sekund.
6. Radio odświeża wskazanie zegara. **Radio samo nic nie liczy** — jeśli
   zmieniarka przestanie wysyłać `0x90`, czas na ekranie zamarznie.

### 11.2. Budowa ramki czasu `0x90` (realny CDX-805, CMD2=0x30) ✅
```
70 31 90 30 61 F1 F0 03 8B D0 0
└─┬─ ─┬─ ─┬ ─┬ ─┬ ─┬ ─┬ ─┬ ─┬ ─┬  └ bajt końca
 RAD  │  CMD1│  │  D1 D2 D3 D4 Parity2
 (70  TAD   CMD2 Parity1
 disp)(31)        =61
```
| Pole | Wartość | Znaczenie (✅ potwierdzone / 🔶 wywnioskowane) |
|:--:|:--:|---|
| RAD | `70` | odbiorca = grupa Display (na ekran) |
| TAD | `31` | nadawca = zmieniarka |
| CMD1 | `90` | „pozycja/ustawienia odtwarzania” |
| CMD2 | `30` | podtyp ramki (lekki „tik” czasu; wariant CDX-805) 🔶 |
| Parity1 | `61` | `70+31+90+30` ✔ |
| **D1** | `F1` | **numer płyty** — `F`= obecna, low nibble = nr (F1=1 … F4=4) ✅ |
| **D2** | `F0` | znacznik/„minuty=0” — w tym przechwycie stały (utwór grał <1 min) 🔶 |
| **D3** | `01..06` | **UPŁYWAJĄCE SEKUNDY — rośnie o 1 co ~1 s** ✅ |
| **D4** | `88` | bajt stanu/sterowania wyświetlaniem 🔶 |
| Parity2 | `CB…D2` | `Parity1 + D1+D2+D3+D4` ✔ |

> **Uwaga o minutach:** w tym przechwyceniu utwór grał tylko kilka sekund, więc
> minuty nie zdążyły się zmienić i pozycja D2 (`F0`) nie została zweryfikowana
> jako licznik minut. Wg generycznej tabeli Mictronics dla `0x90` minuty są w D2,
> sekundy w D3, a numer płyty w D4_hi — ale **realny CDX-805 (CMD2=0x30)** trzyma
> sekundy w D3 i numer płyty w D1. Dlatego dla pewnego liczenia minut bezpiecznie
> jest dodatkowo wysyłać pełny status `0xC0` / `0x97` (patrz §11.4).

### 11.3. Wzorzec z emulatora AVR (dla porównania) ℹ️
Emulator Beckera używa wariantu z `CMD2=0x50` i jawnymi min/sek:
```c
#define DISC_MSG {0x10, ownaddr, 0x90, 0x50, tracknbr, min, sec, (disc-0x10)*16}
//  CMD2=0x50  D1=utwór  D2=min  D3=sek  D4_hi=płyta
```
Czyli różne modele radia/zmieniarki akceptują dwa układy `0x90`:
`CMD2=0x30` (lekki tik sekund, jak CDX-805) oraz `CMD2=0x50` (pełny utwór/min/sek).

### 11.4. Co musi robić emulator, by czas „chodził” ✅
1. Po wejściu w stan **Playing (0x00)** uruchom **timer 1 Hz**.
2. Co sekundę: `sek++` (przy 60 → `sek=0; min++`); aktualizuj też numer utworu.
3. Zgłoś chęć nadania (**slave-break**) i na grant `0x01 0x13` wyślij `0x90`
   z nowym czasem na adres `0x70`.
4. Format minimalny zgodny z CDX-805: `70 <addr> 90 30 <p1> <Fdisc> F0 <sek> <st>`
   (z poprawnymi sumami). Dla pełnej zgodności dorzucaj okresowo `0xC0`/`0x97`
   z liczbą utworów, minutami i sekundami.
5. W **pauzie/seeku** przestań inkrementować (lub wysyłaj `--.--` zgodnie z §9.3);
   po wznowieniu wróć do inkrementacji.
6. Liczby czasu podawaj tak, jak oczekuje radio — w praktyce **BCD** (np. 59 s =
   `0x59`); poniżej 10 hex i BCD są nierozróżnialne, więc bezpiecznie używać BCD.

### 11.5. `0xC0` — status play / pole na ekranie (long word) ✅
Niesie komplet: nr płyty, liczbę utworów, czas, sterowanie ekranem. W trybie
MD/CD (wg Mictronics): `D2_2 = liczba utworów, D2_3 = minuty, D2_4 = sekundy,
D2_5_lo = numer płyty, D2_5_hi = sterowanie wyświetlaniem`. Po `0xC0` zwykle
następuje ramka z **nazwą płyty** (`0xCD`).

W emulatorze AVR „seek/pozycja startowa” to:
```c
#define SEEK_MSG {0x70, ownaddr, 0xC0, 0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x18}
```
wysyłane zaraz po `PLAY (0x20)`.

W naszym logu warianty `0xC0` (numer płyty `F1…F4`, pola `F0 00` = czas ważny,
`FF FF` = skan/odczyt TOC):
```
70 31 C0 40 A1 00 00 00 00 00 F1 F0 00 11 93 0   ; płyta 1, gra
77 31 C0 20 88 00 00 00 00 30 F2 FF FF 88 30 0   ; płyta 2, skan (FFFF)
```

### 11.6. Numer płyty
✅ Kodowany jako górny nibble bajtu „disc” = `F` (obecna), dolny = numer:
`F1`=1, `F2`=2, `F3`=3, `F4`=4. (Stąd w skanie magazynka po starcie widać
sekwencję `F1→F2→F3→F4` = czytanie TOC i CD-TEXT czterech płyt.)
---

## 12. Wytyczne dla emulatora zmieniarki

### 12.1. Warstwa sprzętowa
- Trzy linie: **BUS ON** (wejście — „radio włączone”), **CLOCK** (wejście — zegar
  od mastera), **DATA** (dwukierunkowa, open-collector).
- Nadawanie = ściąganie DATA do masy w takt zegara mastera (urządzenie jest
  **slave SPI-podobny**: master daje zegar, my wystawiamy bity — emulatory używają
  sprzętowego SPI w trybie slave).
- Obsłuż **wake-up** (wymuszenie DATA high budzi mastera) i **BUS ON** jako sygnał
  zasilania.

### 12.2. Warstwa ramki (obowiązkowe)
1. Odbiór: zlicz długość po **CMD1** (`<0x80`=6, `<0xC0`=11, inaczej 16 bajtów).
2. Sprawdź **obie** sumy (Parity1, Parity2) + końcowe `0`; odrzucaj błędne.
3. Reaguj, gdy **RAD == mój przydzielony adres** lub `RAD == 0x18` (broadcast)
   albo `RAD == (moja_grupa | 0)` (broadcast grupowy).
4. Buduj ramki z poprawnymi Parity1/Parity2 (§4.2).
5. Odpowiadaj szybko (rzędu ms; w logu ~6 ms).

### 12.3. DYNAMICZNY adres — krok po kroku
1. Start: `ownaddr = 0x30` (grupa CD, brak ID).
2. Na broadcast **`0x01 0x02` (Anyone)** → wyślij `CD_DEVICE_MSG` (`0x8C…`).
3. Na **Appoint** (`CMD1=0x02`, RAD w mojej grupie `0x3X`) → `ownaddr = RAD`,
   potwierdź `CD_DEVICE_MSG`.
4. Na `0x01 0x00` (reset) → `ownaddr = 0x30` i czekaj na ponowny Anyone.
5. Od tej chwili używaj `ownaddr` jako adresu źródłowego (TAD) i filtra RAD.

> **Nie zakładaj `0x31` na sztywno.** To radio decyduje o dolnym nibble adresu.

### 12.4. Maszyna stanów (minimum, by „grało”)
| Odebrane (RAD=mój / broadcast) | Reakcja |
|---|---|
| `0x01 0x02` Anyone | wyślij device info `0x8C` |
| `0x02` Appoint | zapamiętaj adres, potwierdź `0x8C` |
| `0x01 0x12` time-poll | odeślij `0x00 status` (PONG) |
| `0x01 0x13` request-poll | wyślij zakolejkowany blok (status `0xC0`, info `0x97/0x95`, nazwa `0xC9/0xCD/0xD2`, czas `0x90`) |
| `0x20` PLAY | status=`0x00`, wyślij pozycję `0xC0` |
| `0x24`/`0x25` FF/REW | status=`0x21`, przewijaj; zakończ na broadcast `0x08` |
| `0x26/0x27` next/prev track | zmień utwór, wyślij `0x90` |
| `0x28/0x29` next/prev CD | zmień płytę, status `0x40`→`0x20`, wyślij `0xC0`+nazwy |
| `0xB0` wybór płyty | ustaw płytę/utwór (CMD2=disc, D1=track) |
| `0x84 0xD9/0xDD` żądanie tekstu | odeślij pole nazwy utworu/płyty |
| `0x84 0x95/0x97` żądanie info | odeślij mapę płyt / czas i liczbę utworów |
| broadcast `0xF0 SRC` (nie mój) | przejdź w `0x80` (idle — zdeselekcjonowano) |
| broadcast `0x87 0x00` | power-off → `0x80` (idle) |

### 12.5. Dane do wygenerowania
- **Device info `0x8C`** (grupa CD + liczba slotów) — patrz §6.1.
- **Status `0x00`** (PONG) i mapa stanów (§7.1).
- **Pozycja `0x90`** z numerem utworu/płyty i **sekundami rosnącymi co 1 s**.
- **`0xC0`** status play + `0x97` (czas/utwory) + `0x95` (mapa płyt).
- **Nazwy `0xC9/0xCD`** lub `0xD2` (CD-TEXT) — pola po 8 (lub 6) znaków.

### 12.6. Pułapki
- Długość ramki wynika z **CMD1**, nie z adresu.
- **Parity2** liczy się od Parity1 (nie licz Parity1 podwójnie i nie wliczaj
  końcowego `0`).
- Adres jest **dynamiczny** — obsłuż Anyone/Appoint/Reset.
- Numer płyty to `F|nr` (`F1`=1), nie `0x01`.
- CD-TEXT: pierwszy znak bywa w **CMD2** (wariant `0xD2`).
- Dane „na ekran” wysyłaj na **`0x70`** (display), status/ACK na **`0x10`**
  (master) — z TAD = Twój przydzielony adres.
- Ruch między `0x10`↔`0x18`↔`0x70`↔`0x91` to w dużej części wewnętrzne sprawy
  głowicy (wyświetlacz, DSP, tuner) — emulator je ignoruje.

---

## Załącznik A: format logu snifera

```
[hh:mm:ss.mmm] t=<us> dt=<przerwa_us> bus=<0/1> : B0 B1 ... Bn 00
```
- `t=` — czas wewnętrzny (µs), `dt=` — odstęp od poprzedniej aktywności (µs),
- `bus=` — stan zasilania magistrali, po dwukropku bajty HEX,
- końcowe `00` = bajt kończący word (nie wchodzi do sum),
- linie `BUS=0/1` = zmiana zasilania, `minBitGap=…us` = auto-pomiar,
- `!` przy bajcie = bajt ≠ 8 bitów (błąd),
- samodzielna linia `: 00` = pojedynczy bajt potwierdzenia/wypełnienia.

## Załącznik B: przykłady z pełnym dekodowaniem

```
; --- POLL (short) ---
31 10 01 12 54 0
RAD=31(zmieniarka) TAD=10(master) CMD1=01 CMD2=12(time-poll)
Parity1 = 31+10+01+12 = 54 ✔

; --- STATUS / PONG (short) ---
10 31 00 80 C1 0
RAD=10(master) TAD=31 CMD1=00(status) CMD2=80(idle)
Parity1 = 10+31+00+80 = C1 ✔

; --- POZYCJA 0x90 (middle) ---
70 31 90 30 61 F1 F0 03 8B D0 0
RAD=70(display) TAD=31 CMD1=90 CMD2=30 Parity1=61
D1=F1 D2=F0 D3=03(sek) D4=8B   Parity2 = 61+F1+F0+03+8B = D0 ✔

; --- CD-TEXT 0xD2 (long) ---
70 31 D2 4B BE 61 70 69 74 61 02 00 F1 10 D0 0
RAD=70 TAD=31 CMD1=D2 CMD2='K' Parity1=BE
znaki: K a p i t a = "Kapita"  ; płyta F1
Parity2 = BE+61+70+69+74+61+02+00+F1+10 = D0 ✔

; --- STATUS PLAY 0xC0 (long) ---
70 31 C0 40 A1 00 00 00 00 00 F1 F0 00 11 93 0
RAD=70 TAD=31 CMD1=C0 CMD2=40 Parity1=A1
...(pola statusu, płyta F1)...  Parity2 = 93 ✔

; --- NEXT CD (short, od klawiatury) ---
31 11 28 00 6A 0
RAD=31(zmieniarka) TAD=11(keypad) CMD1=28(Next CD)
Parity1 = 31+11+28+00 = 6A ✔
```

---

## Źródła

Materiały pobrane lokalnie do `./zrodla/`:
- **Mictronics — „Inside Sony Unilink”** (opis magistrali, ramki, adresy):
  <https://www.mictronics.de/posts/Inside-Sony-Unilink/>
- **Mictronics — tablica komend** (`command.htm`):
  <https://www.mictronics.de/command.htm>
- **GNUnilink** (emulator zmieniarki na PIC, model master-poll):
  <https://gnunilink.sourceforge.net/> ; wiki: <http://sophana.free.fr/pmwiki2/index.php5/GNUnilink/Faq>
- **Becker/AVR Unilink** (Michael Wolf) — `unilink.c`/`unilink.h` (procedury
  appoint, status, slave-break, nazwy) — GPL.
- **STM32 „Alfa-166 Unilink CD emulator”** (deividAlfa) — nowszy emulator z
  CD-TEXT/USB; zawiera też kopie dokumentacji Mictronics i logi:
  <https://github.com/deividAlfa/Alfa-166-Unilink-CD-emulator>
- **Sony** — opis złącza BUS / kabla UniLink:
  <https://www.sony.com/electronics/support/articles/00032155>

*Informacje o protokole pochodzą z inżynierii wstecznej prowadzonej przez
społeczność (Mictronics, GNUnilink, autorzy emulatorów) i nie są oficjalną
dokumentacją Sony — używać na własną odpowiedzialność. Treści ze źródeł
zewnętrznych sparafrazowano dla zgodności z ograniczeniami licencyjnymi. Sony i
Unilink to zastrzeżone znaki towarowe Sony Corporation.*

---

## Załącznik C: weryfikacja typów ramek na realnym logu CDX-805

✅ Poniżej **realne ramki** zmieniarki CDX-805 z naszego sniffu, zmapowane na
definicje Mictronics. To gotowy materiał referencyjny dla emulatora (wszystkie
sumy kontrolne się zgadzają).

### C.1. Handshake przydziału adresu (dowód na DYNAMICZNY adres)
Master rozsyła **Appoint** (`CMD1=0x02`) nadając adresy kolejnym urządzeniom:
```
3B 10 02 11 5E 0     ; do urządzenia grupy CD (ID B)
DB 10 02 12 FF 0     ; do zmieniarki MD (grupa D, ID B)
71 10 02 12 95 0     ; do urządzenia grupy Display (ID 1)
31 10 02 14 57 0     ; do NASZEJ zmieniarki -> przydzielony adres 0x31
```
→ To radio przypisało `0x31`. Emulator musi przyjąć **dowolny** ID z grupy `0x3X`.

### C.2. Device info `0x8C` (przed i po przydziale ID)
```
10 30 8C D0 9C 05 A8 1F A3 0B 0   ; TAD=30 (jeszcze bez ID) -> info do mastera
10 31 8C D0 9D 04 A8 1F A3 0B 0   ; TAD=31 (po przydziale)  -> potwierdzenie
  RAD=10(master) CMD1=8C CMD2=D0  Parity1: 10+30+8C+D0=9C ✔ / 10+31+8C+D0=9D ✔
  D1..D4 = 05/04 A8 1F A3  -> łańcuch możliwości CDX-805 (typ/sloty/Custom file)
```
> To jest realny string „jestem zmieniarką CD” konkretnie z CDX-805 — można go
> użyć w emulatorze 1:1 (z własnym TAD = przydzielony adres).

### C.3. Magazynek / mapa płyt
```
70 31 8E C0 EF 00 00 00 80 6F 0   ; 0x8E info magazynka (CMD2: C0/80/F0 = stany)
70 31 95 08 3E 00 00 00 8A C8 0   ; 0x95 mapa obecnych płyt (po skanie magazynka)
90 31 9C 00 5D 00 00 00 88 E5 0   ; 0x9C zmiana płyty -> RAD 0x90; D4_hi=8=brak nazwy
```

### C.4. Identyfikator płyty (disc ID) — `0xC5` / `0xD5`
```
10 31 C5 A2 A8 24 77 52 F0 00 00 00 00 88 0D 0  ; po skanie/zmianie magazynka
10 31 D5 A2 B8 24 77 52 F1 00 00 00 00 88 1E 0  ; przy zmianie płyty w trakcie grania
  CMD1=C5/D5  CMD2=A2 (high 'A' stały, low=1/100 s)
  D1=24 (liczba utworów)  D4=F0 (stały znacznik wg Mictronics ✔)
  reszta = unikatowy nr płyty liczony z danych płyty (do dopasowania CD-TEXT)
```
Radio używa disc ID, by **rozpoznać płytę** i ewentualnie wczytać dla niej
zapamiętaną nazwę/CD-TEXT. Emulator może podać dowolny, ale **stały dla danej
płyty** identyfikator.

### C.5. Pozycja i status (potwierdzenie liczników)
```
70 31 90 30 61 F1 F0 01 88 CB 0   ; sek=01
70 31 90 30 61 F1 F0 02 8B CF 0   ; sek=02  (inkrement co ~1 s -> zegar utworu)
70 31 C0 40 A1 00 00 00 00 00 F1 F0 00 11 93 0  ; status play, płyta F1
77 31 C0 40 A8 00 00 00 00 00 F1 F0 00 21 AA 0  ; ... (RAD=77, drugi kanał display)
```

### C.6. Mapowanie skrótów do sekcji
| Typ z logu | CMD1 | Sekcja opisu |
|---|:--:|---|
| Appoint / dynamiczny adres | `0x02` | §5, §6, C.1 |
| Device info | `0x8C` | §6.1, C.2 |
| Magazynek / mapa płyt | `0x8E`/`0x95`/`0x9C` | §8.2, C.3 |
| Disc ID | `0xC5`/`0xD5` | §8.2, C.4 |
| Pozycja / status | `0x90`/`0xC0` | §11, C.5 |
| CD-TEXT | `0xD2` (i `0xC9/0xCD/0xD9/0xDD`) | §10 |
| Przewijanie | `0x24`/`0x25` + `0x08` | §9 |
