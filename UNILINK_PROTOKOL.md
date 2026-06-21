# Sony UniLink — notatki o protokole

Dokument zbiera to, co **ustaliliśmy z kodu emulatora i z capture'ów magistrali**
(radia MEX-BT3800u oraz CDX-M670 + prawdziwa zmieniarka). Część rzeczy jest
pewna (potwierdzona logami/działaniem), część oznaczona jako **[?]** wymaga
potwierdzenia.

> Skróty: RAD = adres odbiorcy (receiver), TAD = adres nadawcy/celu (target),
> OP1/OP2 = bajty komendy, Pn = suma kontrolna, END = 0x00.

---

## 1. Warstwa fizyczna

Magistrala synchroniczna, sterowana zegarem od radia (master).

| Sygnał   | Kolor (oryg. Sony) | Opis |
|----------|--------------------|------|
| CLOCK    | żółty              | zegar magistrali (radio taktuje każdy bit) |
| DATA     | zielony            | dane dwukierunkowe (open-drain) |
| BUS ON   | niebieski          | zasilanie/aktywność magistrali (HIGH = radio włączone) |
| RESET    | fioletowy          | reset (w tym projekcie nieużywany) |

> Uwaga: w kablu Peiying DB15 oraz w samej zmieniarce kolory bywają **inne**
> (zob. `piny kabla unilink.txt`) — nie sugeruj się kolorem, sprawdzaj sygnał.

Ustawienia warstwy fizycznej w tym projekcie (`Config.h`):
- `PIN_BUS_ON = 4`, `PIN_CLOCK = 5`, `PIN_DATA = 6`
- bit próbkowany na zboczu **RISING** zegara (`CLOCK_EDGE = RISING`)
- linia DATA jest **sprzętowo zanegowana** (`INVERT_DATA = true`)
- bity bajtu nadawane **MSB first** (bit 7 → bit 0)

### Slave Break
Aby urządzenie podrzędne zgłosiło chęć transmisji (np. aktualizacji ekranu),
ściąga linię **DATA do 0 w fazie ciszy** na magistrali. Radio wykrywa break i
odpytuje, kto chce mówić. W tym projekcie:
- break wystawiamy dopiero po `BREAK_SILENCE_US = 8000 us` ciszy,
- trzymamy DATA nisko maks. `BREAK_HOLD_US = 2500 us`,
- **natychmiast porzucamy** break, gdy pojawi się zbocze zegara (radio zaczęło
  nadawać) — inaczej psujemy ramkę radia → SYSTEM RESET.

---

## 2. Struktura ramki

Każda ramka: `RAD TAD OP1 OP2 P1 [dane...] [P2] END`

- **P1** = `(RAD + TAD + OP1 + OP2) & 0xFF`
- **P2** (tylko ramki z danymi) = `(P1 + suma bajtów danych) & 0xFF`
- **END** = `0x00`

Trzy długości używane w emulatorze:

| Typ    | Długość | Układ |
|--------|---------|-------|
| short  | 6 B     | `RAD TAD OP1 OP2 P1 END` |
| medium | 11 B    | `RAD TAD OP1 OP2 P1 D1 D2 D3 D4 P2 END` |
| long   | 16 B    | `RAD TAD OP1 OP2 P1 D1 D2 D3 D4 D5 D6 D7 D8 D9 P2 END` |

Ramki z błędną P1 prawdziwa zmieniarka **ignoruje** (i my też — to odsiewa
kolizje).

---

## 3. Adresy

| Adres | Znaczenie |
|-------|-----------|
| `0x10` | MASTER — radio (źródło ramek sterujących) |
| `0x11` | panel/klawiatura radia (TAD przy komendach z przycisków) |
| `0x14` | procesor ekranu radia (czasem odpytuje status) |
| `0x18` | BROADCAST (ANYONE?, slave poll, reset itp.) |
| `0x31`–`0x3A` | blok adresów zmieniarek; nam radio nadaje zwykle `0x31` |
| `0x3B` | wewnętrzny odtwarzacz CD radia (**tylko CDX-M670**) |
| `0x70` | prefiks ramek z danymi ekranu/statusu od zmieniarki |
| `0x71` | wewnętrzny kontroler radia |
| `0xDB` | wewnętrzne urządzenie pomocnicze (**tylko CDX-M670**) |

> Adres bywa **re-nadawany** po resecie (0x31..0x3A) — adoptujemy każdy nadany.

---

## 4. Stany mechanizmu (bajt statusu)

Raportowane w odpowiedzi na PING (`01 12`) oraz używane do wyboru ekranu:

| Stan  | Znaczenie | Na ekranie radia |
|-------|-----------|------------------|
| `0xC0` | INIT (mechanizm się budzi) | ekran startowy |
| `0x80` | IDLE (gotowy, stoi) | — |
| `0x40` | LOADING (ładowanie płyty) | „LOAD" |
| `0x20` | SEEKING (szukanie utworu) | ekran przejściowy |
| `0x00` | PLAYING (odtwarzanie) | track + czas |

Przejścia: `C0 →(po pierwszym PING + INIT_DURATION) 80`, a po komendzie PLAY:
`40 → 20 → 00`.

---

## 5. Komendy OD radia → zmieniarka (i nasze odpowiedzi)

### Discovery / sesja

| Ramka (RAD TAD OP1 OP2) | Znaczenie | Odpowiedź zmieniarki |
|---|---|---|
| `18 10 01 02` | ANYONE? (broadcast discovery) | atrybuty: `10 30 8C D0 9C 05 A8 1F A3 0B 00` |
| `18 10 01 11` (CDX: też `01 01`) | zapytanie do nieprzydzielonej zmieniarki | „magic": `10 18 04 00 2C 00` (wymusza u radia SYSTEM RESET + właściwe discovery) |
| `18 10 01 00` | SYSTEM RESET (radio zaczyna od nowa) | — (kasujemy adres i stan) |
| `3X 10 02 XX` (X=1..A) | ADDRESS APPOINT — nadanie adresu | status: `10 <addr> 8C D0 <b5> <b6> A8 1F A3 0B 00`, gdzie `b5=0x6C+addr`, `b6=0xA1-b5` |

### Praca bieżąca

| Ramka | Znaczenie | Odpowiedź |
|---|---|---|
| `18 10 01 15` | SLAVE POLL — „kto chce ekran?" | `10 18 82 <typ> 00 00 00 00` (typ: `01` idle, `04` playing, `05` przejściowy) |
| `<addr> 10 01 12` | PING status (od mastera) | `10 <addr> 00 <stan>` |
| `<addr> 14 01 12` | PING status (od procesora ekranu 0x14) | `14 <addr> 00 <stan>` — **trzeba odpowiadać**, inaczej SYSTEM RESET |
| `<addr> 10 01 13` | UPDATE DISPLAY — poproś o ekran | ramka ekranu (zob. §6) |
| `<addr> 10 20 00` | WAKE UP / PLAY | start lub wznowienie odtwarzania |
| `<addr> .. 24` | FAST FORWARD (FF) | przewijanie do przodu |
| `<addr> .. 25` | REWIND (REW) | przewijanie do tyłu |

### Komendy z panelu (`<addr> 11 OP1 OP2`)

| OP1 OP2 | Akcja |
|---------|-------|
| `26 10` | następny utwór |
| `27 10` | poprzedni utwór |
| `28 ..` | następna płyta |
| `29 ..` | poprzednia płyta |

> **FF/REW (0x24/0x25):** potwierdzone w logu — radio CDX-M670 wysyła **jedną**
> ramkę na naciśnięcie (`tad=0x11`, `op2=0x00`, długość 6 B), **bez powtórzeń
> przy przytrzymaniu i bez sygnału zwolnienia**. Nie da się więc wykryć „trzymania".

### Specyficzne dla CDX-M670 (preliminary discovery)

| Ramka | Znaczenie |
|---|---|
| `3B 10 02 11` | appoint wewnętrznego CD radia → marker fazy preliminary |
| `DB 10 02 12` | appoint wewnętrznego urządzenia pomocniczego |

Po tych ramkach przez ~250 ms **ignorujemy ANYONE?** (to faza dla urządzeń
wewnętrznych radia; odpowiedź w tym oknie = pętla SYSTEM RESET).

### Routing audio (diagnostyka)

| Ramka | Znaczenie |
|---|---|
| `18 10 87 XX` | sygnał audio ON/OFF (bit0 OP2 = stan). **Tylko logujemy** — prawdziwa zmieniarka nigdy nie wycisza własnego wyjścia na tej podstawie. |

---

## 6. Ramka ekranu (czas / odtwarzanie)

Odpowiedź na `<addr> 10 01 13` w stanie PLAYING (format z sniffu CDX-M670,
**ten działa** — pokazuje track i czas):

```
70 <addr> C0 00 | P1 | 00 00 00 00 30 <TRK> <MIN> <SEK> <DISC> | P2 | 00
```

Kodowanie pól:
- **TRK** — BCD z F-paddingiem: `F1`..`F9` dla 1–9, potem `10`..`99`
- **MIN** — BCD z F-paddingiem: `F0` dla 0, `F1`..`F9`, potem `10`..`99`
- **SEK** — zwykłe BCD `00`..`59`
- **DISC** — numer płyty w starszym nibblu: `0x10`=CD1 ... `0xA0`=CD10
- bajt `0x30` (D5) to marker stałej tej „strony" (ekran odtwarzania)

> **WAŻNE [potwierdzone empirycznie]:** wariant z `op2=0x40` i `D5=0x00`
> (jaki wysyła prawdziwa zmieniarka, np. `70 31 C0 40 ... F1 F0 00 11`) to
> **INNA STRONA** — radio interpretuje ją jako **LOAD / info płyty**, a NIE jako
> czas. Do pokazania czasu MUSI być `op2=0x00` + marker `0x30`.

---

## 7. Odświeżanie ekranu — co wiemy

- Zmieniarka jest **sterowana odpytywaniem**: nic nie „wypycha", tylko odpowiada
  na `01 13`. Potwierdza to dokumentacja GNUnilink/Cleggy oraz logi.
- Tempo `01 13` ustala **radio** i jest **zmienne**: czasem ~12 Hz (zaraz po
  starcie odtwarzania), w stanie ustalonym bywa ~0,5–1,5 Hz, z przerwami nawet
  kilkusekundowymi.
- W capture **prawdziwej zmieniarki** radio również odpytuje nierówno (przerwy
  1–4 s) — co sugeruje, że radio **samo dolicza sekundy lokalnie** między
  odpytaniami. **[? — do potwierdzenia, czym dokładnie radio jest do tego
  przekonywane; sam `op2=0x40` to nie to, bo daje LOAD]**
- Slave Break może poprosić radio o odpytanie, ale CDX-M670 honoruje to
  niekonsekwentnie; nadmiar breaków (~>3–4 Hz) wywołuje SYSTEM RESET.

---

## 8. Stałe czasowe (dostrojone pod CDX-M670 / MEX-BT3800u)

Z `Config.h` (zmiana którejkolwiek potrafi wywołać pętlę SYSTEM RESET):

| Stała | Wartość | Rola |
|-------|---------|------|
| `INIT_DURATION_MS` | 800 | `C0 → 80` po pierwszym PING |
| `LOAD_DURATION_MS` | 50 | `40 → 20` |
| `SEEK_DURATION_MS` | 50 | `20 → 00` |
| `BREAK_INTERVAL_MS` | 150 | min. odstęp push-break przy zmianie |
| `DISPLAY_KEEPALIVE_MS` | 500 | keepalive break, max ~2 Hz |
| `READ_SILENCE_US` | 5000 | cisza, po której przetwarzamy ramkę RX |
| `BREAK_SILENCE_US` | 8000 | wymagana cisza przed wystawieniem break |
| `BREAK_HOLD_US` | 2500 | jak długo trzymać DATA nisko |
| `FOREIGN_POLL_GUARD_MS` | 30 | okno ochrony, gdy radio odpytuje inne urządzenie |
| `PRELIMINARY_WINDOW_MS` | 250 | ignorowanie ANYONE? po markerze 3B/DB |
| `RADIO_TIMEOUT_MS` | 5000 | brak PING → radio zniknęło |

---

## 9. Przykładowy przebieg discovery (CDX-M670)

```
18 10 01 11  →  10 18 04 00 2C 00     (nieprzydzielona → magic)
18 10 01 00                            (radio: SYSTEM RESET)
3B 10 02 11                            (marker preliminary — ignorujemy ANYONE?)
18 10 01 02  →  (po oknie) 10 30 8C D0 9C 05 A8 1F A3 0B 00   (atrybuty)
31 10 02 14  →  10 31 8C D0 ...        (APPOINT adresu 0x31 → status)
...
18 10 01 15  →  10 18 82 04 ...        (slave poll → „chcę ekran, playing")
31 10 01 13  →  70 31 C0 00 ... 30 ... (display: track + czas)
31 10 01 12  →  10 31 00 00            (PING → stan PLAYING)
```

---

## 10. Otwarte pytania / TODO

1. **Płynny licznik 1 Hz** — która dokładnie część odpowiedzi przekonuje radio,
   by samo doliczało sekundy między odpytaniami? Potrzebny capture **prawdziwej
   zmieniarki podczas faktycznego ODTWARZANIA** (czas rosnący), by zobaczyć
   format ramki czasu i kadencję `01 13` w tym stanie. Dotychczasowe capture'y
   zmieniarki były na `0:00` (bezczynność).
2. **FF/REW** — czy istnieje wariant radia/komenda sygnalizująca trzymanie? Na
   CDX-M670 nie zaobserwowano (jedna ramka na naciśnięcie).
3. Znaczenie bajtów `b5/b6` i `8C D0 ... 0B` w atrybutach/statusie (skąd stałe).
4. Pełne znaczenie „stron" ekranu: `C0 40`, `8E C0`, `8E F0`, `9C` (info płyty,
   tekst, TOC?).
