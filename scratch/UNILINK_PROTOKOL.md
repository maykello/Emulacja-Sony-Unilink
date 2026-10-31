# Sony UniLink ÔÇö notatki o protokole

Dokument zbiera to, co **ustalili┼Ťmy z kodu emulatora i z capture'├│w magistrali**
(radia MEX-BT3800u oraz CDX-M670 + prawdziwa zmieniarka). Cz─Ö┼Ť─ç rzeczy jest
pewna (potwierdzona logami/dzia┼éaniem), cz─Ö┼Ť─ç oznaczona jako **[?]** wymaga
potwierdzenia.

> Skr├│ty: RAD = adres odbiorcy (receiver), TAD = adres nadawcy/celu (target),
> OP1/OP2 = bajty komendy, Pn = suma kontrolna, END = 0x00.

---

## 1. Warstwa fizyczna

Magistrala synchroniczna, sterowana zegarem od radia (master).

| Sygna┼é   | Kolor (oryg. Sony) | Opis |
|----------|--------------------|------|
| CLOCK    | ┼╝├│┼éty              | zegar magistrali (radio taktuje ka┼╝dy bit) |
| DATA     | zielony            | dane dwukierunkowe (open-drain) |
| BUS ON   | niebieski          | zasilanie/aktywno┼Ť─ç magistrali (HIGH = radio w┼é─ůczone) |
| RESET    | fioletowy          | reset (w tym projekcie nieu┼╝ywany) |

> Uwaga: w kablu Peiying DB15 oraz w samej zmieniarce kolory bywaj─ů **inne**
> (zob. `piny kabla unilink.txt`) ÔÇö nie sugeruj si─Ö kolorem, sprawdzaj sygna┼é.

Ustawienia warstwy fizycznej w tym projekcie (`Config.h`):
- `PIN_BUS_ON = 4`, `PIN_CLOCK = 5`, `PIN_DATA = 6`
- bit pr├│bkowany na zboczu **RISING** zegara (`CLOCK_EDGE = RISING`)
- linia DATA jest **sprz─Ötowo zanegowana** (`INVERT_DATA = true`)
- bity bajtu nadawane **MSB first** (bit 7 Ôćĺ bit 0)

### Slave Break
Aby urz─ůdzenie podrz─Ödne zg┼éosi┼éo ch─Ö─ç transmisji (np. aktualizacji ekranu),
┼Ťci─ůga lini─Ö **DATA do 0 w fazie ciszy** na magistrali. Radio wykrywa break i
odpytuje, kto chce m├│wi─ç. W tym projekcie:
- break wystawiamy dopiero po `BREAK_SILENCE_US = 8000 us` ciszy,
- trzymamy DATA nisko maks. `BREAK_HOLD_US = 2500 us`,
- **natychmiast porzucamy** break, gdy pojawi si─Ö zbocze zegara (radio zacz─Ö┼éo
  nadawa─ç) ÔÇö inaczej psujemy ramk─Ö radia Ôćĺ SYSTEM RESET.

---

## 2. Struktura ramki

Ka┼╝da ramka: `RAD TAD OP1 OP2 P1 [dane...] [P2] END`

- **P1** = `(RAD + TAD + OP1 + OP2) & 0xFF`
- **P2** (tylko ramki z danymi) = `(P1 + suma bajt├│w danych) & 0xFF`
- **END** = `0x00`

Trzy d┼éugo┼Ťci u┼╝ywane w emulatorze:

| Typ    | D┼éugo┼Ť─ç | Uk┼éad |
|--------|---------|-------|
| short  | 6 B     | `RAD TAD OP1 OP2 P1 END` |
| medium | 11 B    | `RAD TAD OP1 OP2 P1 D1 D2 D3 D4 P2 END` |
| long   | 16 B    | `RAD TAD OP1 OP2 P1 D1 D2 D3 D4 D5 D6 D7 D8 D9 P2 END` |

Ramki z b┼é─Ödn─ů P1 prawdziwa zmieniarka **ignoruje** (i my te┼╝ ÔÇö to odsiewa
kolizje).

---

## 3. Adresy

| Adres | Znaczenie |
|-------|-----------|
| `0x10` | MASTER ÔÇö radio (┼║r├│d┼éo ramek steruj─ůcych) |
| `0x11` | panel/klawiatura radia (TAD przy komendach z przycisk├│w) |
| `0x14` | procesor ekranu radia (czasem odpytuje status) |
| `0x18` | BROADCAST (ANYONE?, slave poll, reset itp.) |
| `0x31`ÔÇô`0x3A` | blok adres├│w zmieniarek; nam radio nadaje zwykle `0x31` |
| `0x3B` | wewn─Ötrzny odtwarzacz CD radia (**tylko CDX-M670**) |
| `0x70` | prefiks ramek z danymi ekranu/statusu od zmieniarki |
| `0x71` | wewn─Ötrzny kontroler radia |
| `0xDB` | wewn─Ötrzne urz─ůdzenie pomocnicze (**tylko CDX-M670**) |

> Adres bywa **re-nadawany** po resecie (0x31..0x3A) ÔÇö adoptujemy ka┼╝dy nadany.

---

## 4. Stany mechanizmu (bajt statusu)

Raportowane w odpowiedzi na PING (`01 12`) oraz u┼╝ywane do wyboru ekranu:

| Stan  | Znaczenie | Na ekranie radia |
|-------|-----------|------------------|
| `0xC0` | INIT (mechanizm si─Ö budzi) | ekran startowy |
| `0x80` | IDLE (gotowy, stoi) | ÔÇö |
| `0x40` | LOADING (┼éadowanie p┼éyty) | ÔÇ×LOAD" |
| `0x20` | SEEKING (szukanie utworu) | ekran przej┼Ťciowy |
| `0x00` | PLAYING (odtwarzanie) | track + czas |

Przej┼Ťcia: `C0 Ôćĺ(po pierwszym PING + INIT_DURATION) 80`, a po komendzie PLAY:
`40 Ôćĺ 20 Ôćĺ 00`.

---

## 5. Komendy OD radia Ôćĺ zmieniarka (i nasze odpowiedzi)

### Discovery / sesja

| Ramka (RAD TAD OP1 OP2) | Znaczenie | Odpowied┼║ zmieniarki |
|---|---|---|
| `18 10 01 02` | ANYONE? (broadcast discovery) | atrybuty: `10 30 8C D0 9C 05 A8 1F A3 0B 00` |
| `18 10 01 11` (CDX: te┼╝ `01 01`) | zapytanie do nieprzydzielonej zmieniarki | ÔÇ×magic": `10 18 04 00 2C 00` (wymusza u radia SYSTEM RESET + w┼éa┼Ťciwe discovery) |
| `18 10 01 00` | SYSTEM RESET (radio zaczyna od nowa) | ÔÇö (kasujemy adres i stan) |
| `3X 10 02 XX` (X=1..A) | ADDRESS APPOINT ÔÇö nadanie adresu | status: `10 <addr> 8C D0 <b5> <b6> A8 1F A3 0B 00`, gdzie `b5=0x6C+addr`, `b6=0xA1-b5` |

### Praca bie┼╝─ůca

| Ramka | Znaczenie | Odpowied┼║ |
|---|---|---|
| `18 10 01 15` | SLAVE POLL ÔÇö ÔÇ×kto chce ekran?" | `10 18 82 <typ> 00 00 00 00` (typ: `01` idle, `04` playing, `05` przej┼Ťciowy) |
| `<addr> 10 01 12` | PING status (od mastera) | `10 <addr> 00 <stan>` |
| `<addr> 14 01 12` | PING status (od procesora ekranu 0x14) | `14 <addr> 00 <stan>` ÔÇö **trzeba odpowiada─ç**, inaczej SYSTEM RESET |
| `<addr> 10 01 13` | UPDATE DISPLAY ÔÇö popro┼Ť o ekran | ramka ekranu (zob. ┬ž6) |
| `<addr> 10 20 00` | WAKE UP / PLAY | start lub wznowienie odtwarzania |
| `<addr> .. 24` | FAST FORWARD (FF) | przewijanie do przodu |
| `<addr> .. 25` | REWIND (REW) | przewijanie do ty┼éu |

### Komendy z panelu (`<addr> 11 OP1 OP2`)

| OP1 OP2 | Akcja |
|---------|-------|
| `26 10` | nast─Öpny utw├│r |
| `27 10` | poprzedni utw├│r |
| `28 ..` | nast─Öpna p┼éyta |
| `29 ..` | poprzednia p┼éyta |

> **FF/REW (0x24/0x25):** potwierdzone w logu ÔÇö radio CDX-M670 wysy┼éa **jedn─ů**
> ramk─Ö na naci┼Ťni─Öcie (`tad=0x11`, `op2=0x00`, d┼éugo┼Ť─ç 6 B), **bez powt├│rze┼ä
> przy przytrzymaniu i bez sygna┼éu zwolnienia**. Nie da si─Ö wi─Öc wykry─ç ÔÇ×trzymania".

### Specyficzne dla CDX-M670 (preliminary discovery)

| Ramka | Znaczenie |
|---|---|
| `3B 10 02 11` | appoint wewn─Ötrznego CD radia Ôćĺ marker fazy preliminary |
| `DB 10 02 12` | appoint wewn─Ötrznego urz─ůdzenia pomocniczego |

Po tych ramkach przez ~250 ms **ignorujemy ANYONE?** (to faza dla urz─ůdze┼ä
wewn─Ötrznych radia; odpowied┼║ w tym oknie = p─Ötla SYSTEM RESET).

### Routing audio (diagnostyka)

| Ramka | Znaczenie |
|---|---|
| `18 10 87 XX` | sygna┼é audio ON/OFF (bit0 OP2 = stan). **Tylko logujemy** ÔÇö prawdziwa zmieniarka nigdy nie wycisza w┼éasnego wyj┼Ťcia na tej podstawie. |

---

## 6. Ramka ekranu (czas / odtwarzanie)

Odpowied┼║ na `<addr> 10 01 13` w stanie PLAYING (format z sniffu CDX-M670,
**ten dzia┼éa** ÔÇö pokazuje track i czas):

```
70 <addr> C0 00 | P1 | 00 00 00 00 30 <TRK> <MIN> <SEK> <DISC> | P2 | 00
```

Kodowanie p├│l:
- **TRK** ÔÇö BCD z F-paddingiem: `F1`..`F9` dla 1ÔÇô9, potem `10`..`99`
- **MIN** ÔÇö BCD z F-paddingiem: `F0` dla 0, `F1`..`F9`, potem `10`..`99`
- **SEK** ÔÇö zwyk┼ée BCD `00`..`59`
- **DISC** ÔÇö numer p┼éyty w starszym nibblu: `0x10`=CD1 ... `0xA0`=CD10
- bajt `0x30` (D5) to marker sta┼éej tej ÔÇ×strony" (ekran odtwarzania)

> **WA┼╗NE [potwierdzone empirycznie]:** wariant z `op2=0x40` i `D5=0x00`
> (jaki wysy┼éa prawdziwa zmieniarka, np. `70 31 C0 40 ... F1 F0 00 11`) to
> **INNA STRONA** ÔÇö radio interpretuje j─ů jako **LOAD / info p┼éyty**, a NIE jako
> czas. Do pokazania czasu MUSI by─ç `op2=0x00` + marker `0x30`.

---

## 7. Od┼Ťwie┼╝anie ekranu ÔÇö co wiemy

- Zmieniarka jest **sterowana odpytywaniem**: nic nie ÔÇ×wypycha", tylko odpowiada
  na `01 13`. Potwierdza to dokumentacja GNUnilink/Cleggy oraz logi.
- Tempo `01 13` ustala **radio** i jest **zmienne**: czasem ~12 Hz (zaraz po
  starcie odtwarzania), w stanie ustalonym bywa ~0,5ÔÇô1,5 Hz, z przerwami nawet
  kilkusekundowymi.
- W capture **prawdziwej zmieniarki** radio r├│wnie┼╝ odpytuje nier├│wno (przerwy
  1ÔÇô4 s) ÔÇö co sugeruje, ┼╝e radio **samo dolicza sekundy lokalnie** mi─Ödzy
  odpytaniami. **[? ÔÇö do potwierdzenia, czym dok┼éadnie radio jest do tego
  przekonywane; sam `op2=0x40` to nie to, bo daje LOAD]**
- Slave Break mo┼╝e poprosi─ç radio o odpytanie, ale CDX-M670 honoruje to
  niekonsekwentnie; nadmiar break├│w (~>3ÔÇô4 Hz) wywo┼éuje SYSTEM RESET.

---

## 8. Sta┼ée czasowe (dostrojone pod CDX-M670 / MEX-BT3800u)

Z `Config.h` (zmiana kt├│rejkolwiek potrafi wywo┼éa─ç p─Ötl─Ö SYSTEM RESET):

| Sta┼éa | Warto┼Ť─ç | Rola |
|-------|---------|------|
| `INIT_DURATION_MS` | 800 | `C0 Ôćĺ 80` po pierwszym PING |
| `LOAD_DURATION_MS` | 50 | `40 Ôćĺ 20` |
| `SEEK_DURATION_MS` | 50 | `20 Ôćĺ 00` |
| `BREAK_INTERVAL_MS` | 150 | min. odst─Öp push-break przy zmianie |
| `DISPLAY_KEEPALIVE_MS` | 500 | keepalive break, max ~2 Hz |
| `READ_SILENCE_US` | 5000 | cisza, po kt├│rej przetwarzamy ramk─Ö RX |
| `BREAK_SILENCE_US` | 8000 | wymagana cisza przed wystawieniem break |
| `BREAK_HOLD_US` | 2500 | jak d┼éugo trzyma─ç DATA nisko |
| `FOREIGN_POLL_GUARD_MS` | 30 | okno ochrony, gdy radio odpytuje inne urz─ůdzenie |
| `PRELIMINARY_WINDOW_MS` | 250 | ignorowanie ANYONE? po markerze 3B/DB |
| `RADIO_TIMEOUT_MS` | 5000 | brak PING Ôćĺ radio znikn─Ö┼éo |

---

## 9. Przyk┼éadowy przebieg discovery (CDX-M670)

```
18 10 01 11  Ôćĺ  10 18 04 00 2C 00     (nieprzydzielona Ôćĺ magic)
18 10 01 00                            (radio: SYSTEM RESET)
3B 10 02 11                            (marker preliminary ÔÇö ignorujemy ANYONE?)
18 10 01 02  Ôćĺ  (po oknie) 10 30 8C D0 9C 05 A8 1F A3 0B 00   (atrybuty)
31 10 02 14  Ôćĺ  10 31 8C D0 ...        (APPOINT adresu 0x31 Ôćĺ status)
...
18 10 01 15  Ôćĺ  10 18 82 04 ...        (slave poll Ôćĺ ÔÇ×chc─Ö ekran, playing")
31 10 01 13  Ôćĺ  70 31 C0 00 ... 30 ... (display: track + czas)
31 10 01 12  Ôćĺ  10 31 00 00            (PING Ôćĺ stan PLAYING)
```

---

## 10. Otwarte pytania / TODO

1. **P┼éynny licznik 1 Hz** ÔÇö kt├│ra dok┼éadnie cz─Ö┼Ť─ç odpowiedzi przekonuje radio,
   by samo dolicza┼éo sekundy mi─Ödzy odpytaniami? Potrzebny capture **prawdziwej
   zmieniarki podczas faktycznego ODTWARZANIA** (czas rosn─ůcy), by zobaczy─ç
   format ramki czasu i kadencj─Ö `01 13` w tym stanie. Dotychczasowe capture'y
   zmieniarki by┼éy na `0:00` (bezczynno┼Ť─ç).
2. **FF/REW** ÔÇö czy istnieje wariant radia/komenda sygnalizuj─ůca trzymanie? Na
   CDX-M670 nie zaobserwowano (jedna ramka na naci┼Ťni─Öcie).
3. Znaczenie bajt├│w `b5/b6` i `8C D0 ... 0B` w atrybutach/statusie (sk─ůd sta┼ée).
4. Pe┼éne znaczenie ÔÇ×stron" ekranu: `C0 40`, `8E C0`, `8E F0`, `9C` (info p┼éyty,
   tekst, TOC?).
