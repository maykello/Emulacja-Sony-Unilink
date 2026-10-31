# Sony Unilink ÔÇö kompendium protoko┼éu magistrali

Dokumentacja techniczna pod budow─Ö **emulatora zmieniarki CD** dla radia Sony.

Opracowano na podstawie:
1. **Pasywnego sniffu** komunikacji radia **Sony CDX-M670** ze zmieniark─ů
   **CDX-805** (plik `unilink_log_zmieniarka_CDX-M670_20260605_165726.txt`,
   sniffer ÔÇ×Sony UniLink SPY v2ÔÇŁ, ESP32-S3),
2. **Dokumentacji Mictronics** ÔÇ×Inside Sony UnilinkÔÇŁ oraz tablicy komend
   (`command.htm`) ÔÇö najpe┼éniejszego publicznego opisu protoko┼éu (wynik
   in┼╝ynierii wstecznej spo┼éeczno┼Ťci unilink),
3. **Dzia┼éaj─ůcych implementacji emulator├│w** open-source:
   - Becker/AVR Unilink (Michael Wolf) ÔÇö `unilink.c`,
   - STM32 ÔÇ×Alfa-166 Unilink CD emulatorÔÇŁ (deividAlfa) ÔÇö nowszy, z CD-TEXT i USB.

Wszystkie pobrane materia┼éy le┼╝─ů w podkatalogu `./zrodla/` (HTML dokumentacji
i kod ┼║r├│d┼éowy emulator├│w).

> **Znaczniki wiarygodno┼Ťci:**
> - Ôťů **Potwierdzone** ÔÇö zgodne mi─Ödzy logiem, dokumentacj─ů Mictronics i kodem
>   emulator├│w (zweryfikowane te┼╝ arytmetycznie).
> - ­čöÂ **Wywnioskowane** ÔÇö z obserwacji; prawdopodobne, lecz niegwarantowane dla
>   ka┼╝dego modelu radia.
> - Ôä╣´ŞĆ **T┼éo / wariant** ÔÇö informacja zale┼╝na od modelu urz─ůdzenia.
>
> Tre┼Ťci ze ┼║r├│de┼é zewn─Ötrznych sparafrazowano dla zgodno┼Ťci z licencjami.

---

## Spis tre┼Ťci
1. [Warstwa fizyczna ÔÇö trzy linie i zegar](#1-warstwa-fizyczna--trzy-linie-i-zegar)
2. [Stany magistrali, wybudzanie i slave-break](#2-stany-magistrali-wybudzanie-i-slave-break)
3. [Struktura ramki (RAD/TAD/CMD/parzysto┼Ťci)](#3-struktura-ramki-radtadcmdparzysto┼Ťci)
4. [Dwie sumy kontrolne ÔÇö dok┼éadnie](#4-dwie-sumy-kontrolne--dok┼éadnie)
5. [Adresowanie: grupy i DYNAMICZNY przydzia┼é ID](#5-adresowanie-grupy-i-dynamiczny-przydzia┼é-id)
6. [Inicjalizacja i przydzia┼é adresu (Initial Link / Appoint)](#6-inicjalizacja-i-przydzia┼é-adresu-initial-link--appoint)
7. [Cykl pracy: time-poll, request-poll, status](#7-cykl-pracy-time-poll-request-poll-status)
8. [Tablica komend](#8-tablica-komend)
9. [Przewijanie i sterowanie odtwarzaniem (szczeg├│┼éowo)](#9-przewijanie-i-sterowanie-odtwarzaniem-szczeg├│┼éowo)
10. [CD-TEXT i nazwy p┼éyt/utwor├│w (szczeg├│┼éowo)](#10-cd-text-i-nazwy-p┼éytutwor├│w-szczeg├│┼éowo)
11. [Status odtwarzania i pozycja (0x90 / 0xC0)](#11-status-odtwarzania-i-pozycja-0x90--0xc0)
12. [Wytyczne dla emulatora zmieniarki](#12-wytyczne-dla-emulatora-zmieniarki)
13. [Za┼é─ůcznik A: format logu snifera](#za┼é─ůcznik-a-format-logu-snifera)
14. [Za┼é─ůcznik B: przyk┼éady z pe┼énym dekodowaniem](#za┼é─ůcznik-b-przyk┼éady-z-pe┼énym-dekodowaniem)
15. [┼╣r├│d┼éa](#┼║r├│d┼éa)

---

## 1. Warstwa fizyczna ÔÇö trzy linie i zegar

Ôťů Unilink to **synchroniczna magistrala z zegarem od mastera** (a nie czysty
single-wire PWM jak pokrewny S-Link). Wed┼éug dokumentacji Mictronics i zgodnie
z obserwowanym sniffem:

- Sygna┼éy **TTL: 0 V = LOW, 5 V = HIGH** (sniffer ESP32-S3 wymaga konwersji
  5 V Ôćĺ 3,3 V).
- Magistrala ma **trzy linie** sygna┼éowe (plus masa i zasilanie):
  | Linia | Rola |
  |---|---|
  | **BUS ON** | zasilanie/aktywacja magistrali (master j─ů w┼é─ůcza) |
  | **CLOCK** | zegar taktuj─ůcy, generuje **master** dla wszystkich slave'├│w |
  | **DATA** | dwukierunkowa linia danych (open-collector, wired-OR) |

- **Taktowanie bitu:** okres zegara ~**20 ┬Ás**, wype┼énienie 50%. Dana na linii
  DATA zmienia si─Ö w po┼éowie wysokiego stanu zegara.
- **Tempo bajt├│w:** zegar ÔÇ×bajtowyÔÇŁ ~1 ms Ôćĺ **jeden bajt na ~1 ms**.
- Na pocz─ůtku ka┼╝dego nowego pakietu jest **~3 ms** przytrzymania DATA w stanie
  niskim (znacznik startu pakietu).

> W logu snifera wida─ç to po┼Ťrednio: odst─Öpy `dtÔëł6000 ┬Ás` mi─Ödzy kolejnymi
> ramkami transakcji oraz `dtÔëł0,5ÔÇô0,6 s` mi─Ödzy rundami odpytywania w spoczynku.
> Sniffer mierzy `minBitGap` (raportuje `1us`).

---

## 2. Stany magistrali, wybudzanie i slave-break

Ôťů Magistrala jest zawsze w jednym z trzech stan├│w:
- **off** ÔÇö `BUS ON = 0`, linie CLOCK/DATA bez znaczenia (tryb u┼Ťpienia).
- **idle** ÔÇö `BUS ON = 1`, `CLOCK = 0`, a DATA generuje **fal─Ö prostok─ůtn─ů**:
  ~8 ms low, potem ~8 ms high (utrzymywan─ů przez mastera).
- **active** ÔÇö `BUS ON = 1`, trwa transmisja taktowana zegarem.

### 2.1. Wybudzanie (Wake Up)
­čöÂ/Ôťů Slave mo┼╝e **obudzi─ç** mastera, wymuszaj─ůc stan **wysoki** na linii DATA.
Master odpowiada **w┼é─ůczeniem linii BUS ON**. W logu pocz─ůtek to pulsowanie
`BUS=1/0`, a┼╝ magistrala ustala `bus=1` i rusza komunikacja.

### 2.2. Slave-break (slave chce nada─ç)
Ôťů Gdy urz─ůdzenie podrz─Ödne ma dane do wys┼éania, sygnalizuje to **slave-break**
w trakcie stanu idle (kod emulator├│w potwierdza dok┼éadne czasy):
1. odczekaj, a┼╝ DATA jest **low Ôëą ~7ÔÇô8 ms** (magistrala w idle),
2. odczekaj ~**2 ms** na stan **high** DATA (nadal idle),
3. **wymu┼Ť DATA low na ~3 ms**,
4. zwolnij DATA do high na ostatnie ~3 ms.

Ca┼éy slave-break mie┼Ťci si─Ö w jednym ~8 ms wysokim oknie idle. Po nim master
wie, ┼╝e kto┼Ť chce m├│wi─ç, i uruchamia **request-poll** (patrz ┬ž7), pytaj─ůc ÔÇ×kto
chce m├│wi─çÔÇŁ (`0x01 0x15`), a nast─Öpnie daje pozwolenie (`0x01 0x13`).

---

## 3. Struktura ramki (RAD/TAD/CMD/parzysto┼Ťci)

Ôťů Ka┼╝dy ÔÇ×wordÔÇŁ (ramka) ma jeden z **trzech rozmiar├│w**, a rozmiar wyznacza
**warto┼Ť─ç CMD1** (3. bajt). Regu┼éa z kodu emulator├│w (potwierdzona w logu):

| CMD1 | Rozmiar ramki | Nazwa |
|---|---|---|
| `CMD1 < 0x80` | **6 bajt├│w** | short word |
| `0x80 ÔëĄ CMD1 < 0xC0` | **11 bajt├│w** | middle word |
| `CMD1 Ôëą 0xC0` | **16 bajt├│w** | long word |

(Rozmiary liczone wraz z ko┼äcowym bajtem `0`.)

### 3.1. Uk┼éady ramek
```
short  (6B):  RAD TAD CMD1 CMD2 Parity1 0
middle (11B): RAD TAD CMD1 CMD2 Parity1 D1 D2 D3 D4 Parity2 0
long   (16B): RAD TAD CMD1 CMD2 Parity1 D1 D2 D3 D4 D2_1 D2_2 D2_3 D2_4 D2_5 Parity2 0
```

| Pole | Znaczenie |
|---|---|
| **RAD** | *Receiver address* ÔÇö adres odbiorcy (w mojej wcze┼Ťniejszej notacji ÔÇ×DESTÔÇŁ). |
| **TAD** | *Transmitter address* ÔÇö adres nadawcy (ÔÇ×SRCÔÇŁ). |
| **CMD1** | g┼é├│wny bajt komendy (zarazem wyznacza d┼éugo┼Ť─ç ramki). |
| **CMD2** | podkomenda **albo** pierwszy bajt danych (zale┼╝nie od CMD1). |
| **Parity1** | suma kontrolna nag┼é├│wka (patrz ┬ž4). |
| **D1ÔÇôD4** | cztery bajty danych (middle/long). |
| **D2_1ÔÇôD2_5** | kolejne pi─Ö─ç bajt├│w danych (tylko long). |
| **Parity2** | druga suma kontrolna (patrz ┬ž4). |
| **0** | bajt zerowy ko┼äcz─ůcy **ka┼╝dy** word. |

> To dok┼éadnie pokrywa si─Ö z moj─ů wcze┼Ťniejsz─ů, empiryczn─ů analiz─ů logu
> (`DEST SRC CMD D0 [HDR-CHK ÔÇŽ] CHK 00`) ÔÇö Mictronics nadaje tym polom oficjalne
> nazwy. ÔÇ×ParityÔÇŁ to myl─ůce okre┼Ťlenie: w istocie to **sumy kontrolne**.

---

## 4. Dwie sumy kontrolne ÔÇö dok┼éadnie

Ôťů Zweryfikowane arytmetycznie na ramkach z logu oraz zgodne z kodem emulator├│w:

- **Parity1** = `(RAD + TAD + CMD1 + CMD2) mod 256`
  ÔÇö suma czterech bajt├│w nag┼é├│wka.
- **Parity2** = `(suma WSZYSTKICH bajt├│w danych przed ni─ů, ┼é─ůcznie z tymi, kt├│re
  ju┼╝ wesz┼éy do Parity1, ale BEZ samego Parity1) mod 256`
  ÔÇö czyli `(RAD+TAD+CMD1+CMD2 + D1+ÔÇŽ+D2_5) mod 256`.

R├│wnowa┼╝nie (wygodne w implementacji):
```
Parity1 = (RAD + TAD + CMD1 + CMD2) & 0xFF
Parity2 = (Parity1 + D1 + D2 + ... + ostatni_bajt_danych) & 0xFF
```
W **short word** nie ma Parity2 ÔÇö Parity1 jest jedyn─ů sum─ů (i pokrywa si─Ö z ni─ů).

### 4.1. Weryfikacja na realnych ramkach (z logu)
```
3B 10 02 11 5E 0                  ; short
  Parity1 = 3B+10+02+11 = 5E  Ôťö

18 70 8C 00 |14| 80 05 0A 01 A4 0  ; middle (CMD1=8C Ôëą0x80)
  Parity1(=byte4) = 18+70+8C+00 = 14  Ôťö
  Parity2(=A4)    = 14+80+05+0A+01 = A4  Ôťö

70 31 D2 4B |BE| 61 70 69 74 61 02 00 F1 10 D0 0  ; long (CMD1=D2 Ôëą0xC0)
  Parity1(=BE) = 70+31+D2+4B = BE  Ôťö
  Parity2(=D0) = BE+61+70+69+74+61+02+00+F1+10 = D0  Ôťö
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
Bajt ko┼äcowy `0` **nie** wchodzi do ┼╝adnej sumy.
---

## 5. Adresowanie: grupy i DYNAMICZNY przydzia┼é ID

To jedna z najwa┼╝niejszych poprawek wzgl─Ödem pierwszej wersji dokumentu.

Ôťů **Adres = dwa nibble:**
- **g├│rny nibble = GRUPA urz─ůdzenia** (typ, sta┼éy, wynika z rodzaju sprz─Ötu),
- **dolny nibble = ID urz─ůdzenia w grupie** ÔÇö **przydzielany DYNAMICZNIE przez
  mastera** podczas inicjalizacji (Initial Link).

> **DLATEGO emulator NIE ma ÔÇ×na sztywnoÔÇŁ adresu 0x31.** Zmieniarka nale┼╝y do
> **grupy `0x3` (CD players/changers)**, ale konkretny adres (`0x30`, `0x31`,
> `0x32`, ÔÇŽ) **nadaje radio** przy starcie. W naszym logu akurat wypad┼éo `0x31`
> (grupa 3, ID 1), ale przy innym zestawie urz─ůdze┼ä mo┼╝e to by─ç inny ID. Emulator
> musi **zg┼éosi─ç swoj─ů grup─Ö** i **przyj─ů─ç ID** przydzielony przez mastera, a
> potem u┼╝ywa─ç go jako swojego adresu. (Kod emulator├│w startuje z `ownaddr=0x30`
> = ÔÇ×grupa CD, brak IDÔÇŁ i nadpisuje go przydzielonym adresem ÔÇö patrz ┬ž6.)

Ôťů Je┼Ťli w polu **RAD** dolny nibble = `0`, to **broadcast do ca┼éej grupy**
(np. `0x30` = do wszystkich urz─ůdze┼ä CD).

### 5.1. Grupy urz─ůdze┼ä (g├│rny nibble)
| Grupa | Typ urz─ůdzenia |
|:--:|---|
| `0x3` | **Odtwarzacze / zmieniarki CD** ÔćÉ nasza grupa |
| `0x5` | Tunery |
| `0x6` | Magnetofony kasetowe |
| `0x7` | Wy┼Ťwietlacz (Display) |
| `0x8` | Multiplekser audio/bus (np. XA-C30) |
| `0xC` | Zegar |
| `0xD` | Zmieniarki MD |

### 5.2. Adresy specjalne i pozosta┼ée
| Adres | Rola |
|:--:|---|
| **`0x10`** | **Master** (radio / g┼éowica) ÔÇö odpytuje, odbiera ACK i odpowiedzi |
| **`0x18`** | **Broadcast** (do wszystkich) ÔÇö st─ůd cz─Öste `18 10 01 ÔÇŽ` = rozg┼éoszeniowy poll |
| **`0x70`** | **Broadcast do grupy Display** (id 0) ÔÇö tu slave'y ÔÇ×zrzucaj─ůÔÇŁ dane do pokazania na ekranie |
| `0x11` | Klawiatura (Keypad) |
| `0x21` | Klawiatura (sterowanie) |
| `0x91` | DSP (g┼éo┼Ťno┼Ť─ç, barwa itp.) |

### 5.3. Korekta wcze┼Ťniejszych wniosk├│w (z analizy samego logu)
Surowa analiza logu sugerowa┼éa role, kt├│re dokumentacja Mictronics doprecyzowuje:
| Adres w logu | Pierwotny wniosek | **Poprawnie (Mictronics)** |
|:--:|---|---|
| `0x18` | ÔÇ×panel przedniÔÇŁ | **Broadcast** (adres rozg┼éoszeniowy) |
| `0x70`/`0x77` | ÔÇ×punkt danych g┼éowicyÔÇŁ | **Broadcast/element grupy Display `0x7`** |
| `0x31` | ÔÇ×zmieniarkaÔÇŁ | Ôťů Zmieniarka: **grupa 3, ID 1 (przydzielone)** |
| `0x3B` | ÔÇ×kana┼é obecno┼ŤciÔÇŁ | inne urz─ůdzenie **grupy CD** (ID B) |
| `0x71` | ÔÇ×kana┼é zmieniarkiÔÇŁ | urz─ůdzenie **grupy Display** (ID 1), **nie** zmieniarka |
| `0x51` | ÔÇ×tunerÔÇŁ | Ôťů Tuner (grupa 5) |
| `0x91` | ÔÇ×DSPÔÇŁ | Ôťů DSP |
| `0xDB` | ÔÇ×adres tymczasowyÔÇŁ | **zmieniarka MD** (grupa D, ID B) |

---

## 6. Inicjalizacja i przydzia┼é adresu (Initial Link / Appoint)

Ôťů Procedura wykrywania i nadawania adres├│w (Mictronics + kod emulator├│w):

1. **Wake up** ÔÇö magistrala si─Ö budzi (`BUS ON = 1`).
2. **ÔÇ×Anyone?ÔÇŁ** ÔÇö master rozsy┼éa `CMD1=0x01, CMD2=0x02` (*anyone / initial link*).
3. **Zg┼éoszenie typu** ÔÇö slave bez ID (zmieniarka startuje z `ownaddr=0x30`)
   odpowiada **stringiem informacyjnym o urz─ůdzeniu** (`CMD1=0x8C`) ÔÇö m├│wi
   masterowi, do jakiej grupy nale┼╝y i jakie ma mo┼╝liwo┼Ťci.
4. **Przydzia┼é (Appoint)** ÔÇö master wysy┼éa pakiet do **mojej grupy** z konkretnym
   adresem (np. RAD=`0x31`). Slave **zapami─Ötuje nowy adres** i potwierdza,
   ponownie wysy┼éaj─ůc sw├│j string informacyjny.
5. **ÔÇ×Appoint endÔÇŁ** ÔÇö master ko┼äczy `CMD1=0x01, CMD2=0x04`.
6. Master sprawdza `Anyone special (0x03)` / `Link On`, czy s─ů kolejne urz─ůdzenia.

### 6.1. String informacyjny o urz─ůdzeniu (`0x8C`) Ôťů
Z dokumentacji Mictronics (przyk┼éady ÔÇ×device infoÔÇŁ):
```
CMD1 CMD2  D1    D2    D3    D4
0x8C 0xA0  0x06  0xA8  0x25  0xA0   ; zmieniarka CDX-605 / CDX-91
0x8C 0x89  0x15  0xAC  0x17  0xA0   ; zmieniarka CDX-71
0x8C 0x10  0x24  0xA8  0x17  0xA0   ; przyk┼éad ÔÇ×device infoÔÇŁ (u┼╝yty w emulatorze AVR)
0x8C 0x00  0x24  0x2C  0x22  0xA0   ; nap─Öd MD zewn─Ötrzny
```
Znaczenie bajt├│w (­čöÂ wg Mictronics):
- **D2:** `0001xxxx` = urz─ůdzenie wewn─Ötrzne (1 slot), `0010xxxx` = zewn─Ötrzne.
- **D3:** `0001xxxx` = ÔÇ×Custom fileÔÇŁ, `0010xxxx` = brak Custom file.
- **D4:** `1111 0000` ÔÇö g├│rne bity = **liczba slot├│w na p┼éyty**.

W emulatorze AVR string to:
```c
#define CD_DEVICE_MSG {0x10, ownaddr, 0x8C, 0x10, 0x24, 0xA8, 0x17, 0xA0}
```
(wysy┼éany do mastera `0x10`).

### 6.2. Obs┼éuga ÔÇ×AnyoneÔÇŁ i ÔÇ×AppointÔÇŁ w emulatorze (wzorzec) Ôťů
```
JE┼╗ELI RAD == 0x18 (broadcast):
    JE┼╗ELI CMD1==0x01 i CMD2==0x02 (Anyone) i nie mam ID:
        wy┼Ťlij CD_DEVICE_MSG     // zg┼éo┼Ť grup─Ö CD
    JE┼╗ELI CMD1==0x01 i CMD2==0x00 (Bus reset):
        ownaddr = 0x30           // skasuj przydzielony ID

JE┼╗ELI (ownaddr == 0x30) i CMD1==0x02 i (RAD & 0xF0)==0x30 (Appoint do mojej grupy):
    ownaddr = RAD                // ZAPAMI─śTAJ przydzielony adres (np. 0x31)
    wy┼Ťlij CD_DEVICE_MSG         // potwierd┼║
```

---

## 7. Cykl pracy: time-poll, request-poll, status

Ôťů Po inicjalizacji master cyklicznie odpytuje urz─ůdzenia (`CMD1=0x01`).
Podkomendy (CMD2) ÔÇö z dokumentacji i nag┼é├│wka emulatora:

| CMD1 | CMD2 | Znaczenie |
|:--:|:--:|---|
| `0x01` | `0x00` | Re-initialize bus (reset) |
| `0x01` | `0x02` | **Anyone?** (initial link) |
| `0x01` | `0x03` | Anyone special? |
| `0x01` | `0x04` | Appoint end |
| `0x01` | `0x11` | Time poll end |
| `0x01` | `0x12` | **Time poll** ÔÇö slave odpowiada **statusem** (PONG) |
| `0x01` | `0x13` | **Request time poll** ÔÇö pozwolenie na nadanie danych |
| `0x01` | `0x15` | ÔÇ×Kto chce m├│wi─ç?ÔÇŁ (po slave-break) |

### 7.1. Odpowied┼║ na time-poll (status, PONG) Ôťů
Na `0x01 0x12` slave odsy┼éa **short word** `CMD1=0x00, CMD2=status`:
```c
#define STATUS_MSG {0x10, ownaddr, 0x00, unilink_status}
```
Warto┼Ťci **status** (CMD2 przy `CMD1=0x00`, ÔÇ×Status from slaveÔÇŁ):
| Status | Znaczenie |
|:--:|---|
| `0x00` | Playing (odtwarzanie) |
| `0x01` | Playing (wariant) |
| `0x20` | Changed CD (zmieniono p┼éyt─Ö) |
| `0x21` | **Seeking (przewijanie)** |
| `0x40` | Changing CD (trwa zmiana p┼éyty) |
| `0x80` | Idle (bezczynno┼Ť─ç) |
| `0xC0` | Ejecting |
| `0xF0` | Remote Joystick key |
| `0xFA` | Drugi poziom klawisza SEEK |

­čöÂ Typowa ÔÇ×maszynkaÔÇŁ stan├│w w emulatorze po kolejnych time-pollach:
`0x40 (changing) Ôćĺ 0x20 (changed) Ôćĺ 0x21 (seeking) Ôćĺ 0x00 (playing)`.

### 7.2. Pozwolenie na nadanie (`0x01 0x13`) Ôťů
Gdy slave wcze┼Ťniej zrobi┼é **slave-break** (mia┼é co┼Ť do powiedzenia), master daje
mu g┼éos przez `0x01 0x13`. Slave wysy┼éa wtedy **zakolejkowan─ů** ramk─Ö: status
p┼éyty (`0xC0`), info magazynka (`0x8E/0x95`), nazw─Ö (`0xC9/0xCD/0xD2`), czas
(`0x90`) itd.
---

## 8. Tablica komend

Ôťů Zestawienie najwa┼╝niejszych komend (`CMD1`) wg dokumentacji Mictronics
(`command.htm`), potwierdzonych w logu i kodzie emulator├│w. Kolumna ÔÇ×TypÔÇŁ:
`m` = od mastera, `s` = do/od slave (steruj─ůce), `l` = long word z danymi.

### 8.1. Komendy steruj─ůce (master Ôćĺ zmieniarka)
| CMD1 | CMD2 | Funkcja | Uwagi |
|:--:|:--:|---|---|
| `0x01` | (pkt 7) | System/poll | ÔÇ×0x01ÔÇŽÔÇŁ to komendy systemowe ÔÇö b┼é─ůd CRC = pe┼éna reinicjalizacja |
| `0x02` | `0x01` | Konfiguracja: przydziel ID | Appoint |
| `0x08` | `0x00` | **Key off (Cancel)** | wysy┼éane po **puszczeniu** klawisza (ko┼äczy FF/REW) |
| `0x20` | ÔÇö | **PLAY** | start odtwarzania |
| `0x24` | ÔÇö | **Fast Forward (przewijanie w prz├│d)** | start; **koniec** = broadcast `0x08` |
| `0x25` | ÔÇö | **Fast Reverse (przewijanie w ty┼é)** | start; **koniec** = broadcast `0x08` |
| `0x26` | ÔÇö | **Next Track** (nast─Öpny utw├│r) | |
| `0x27` | ÔÇö | **Previous Track** (poprzedni utw├│r) | |
| `0x28` | ÔÇö | **Next CD** (nast─Öpna p┼éyta) | |
| `0x29` | ÔÇö | **Previous CD** (poprzednia p┼éyta) | |
| `0x34` | ÔÇö | Toggle Repeat (CD/MD) | prze┼é─ůcza tryby repeat |
| `0x35` | ÔÇö | Toggle Shuffle | |
| `0x36` | ÔÇö | Toggle Intro | |
| `0x37` | ÔÇö | Toggle Bank | |
| `0x41` | `0x00/40/50` | Edycja nazwy p┼éyty (kursor / znak +/Ôłĺ) | tryb edycji nazwy |
| `0x84` | `0xD9/0xDD/0x95/0x97` | **┼╗─ůdanie p├│l tekstu / info** | patrz ┬ž10 (CD-TEXT) |
| `0xB0` | disc | **Bezpo┼Ťredni wyb├│r p┼éyty**; D1 = utw├│r | ÔÇ×Set positionÔÇŁ |

### 8.2. Komendy/odpowiedzi od zmieniarki (Ôćĺ master `0x10` lub display `0x70`)
| CMD1 | Funkcja | Kluczowe pola |
|:--:|---|---|
| `0x00` | **Status** (odpowied┼║ na time-poll) | CMD2 = status (┬ž7.1) |
| `0x8C` | Device info / ÔÇ×anyone respondÔÇŁ | identyfikacja urz─ůdzenia (┬ž6.1) |
| `0x8E` | Info magazynka/kasety | `0x40` slot pusty, `0x80` w┼éo┼╝ono magazynek, `0xC0` brak magazynka |
| `0x90` | **Pozycja odtwarzania** | D1=utw├│r, D2=min, D3=sek, D4_hi=p┼éyta (┬ž11) |
| `0x91` | Bie┼╝─ůcy czas / znaki nawigacji | |
| `0x94` | Tryby Repeat/Shuffle/Intro/Bank (ikony) | bitowe pola D1ÔÇôD3 |
| `0x95` | Mapa obecno┼Ťci p┼éyt | bit = p┼éyta obecna (po skanie magazynka) |
| `0x97` | Info p┼éyty | D1=liczba utwor├│w, D2=min, D3=sek, D4_hi=p┼éyta |
| `0x98` | Koniec Intro play | |
| `0x9C` | Zmiana p┼éyty Ôćĺ RAD `0x90` | D4_lo=nr p┼éyty, D4_hi: 8=brak nazwy, C=nazwa dost─Öpna |
| `0xC0` | **Status play / pole tunera** (long) | dysk, min, sek, sterowanie ekranem (┬ž11) |
| `0xC1` | Info PTY stacji (long, tuner) | |
| `0xC5` | **Identyfikator p┼éyty (disc ID)** ÔÇö po skanie/zmianie magazynka | unikatowy nr p┼éyty |
| `0xC9` | **Nazwa utworu** (pole 0ÔÇô1) | ASCII (┬ž10) |
| `0xCD` | **Nazwa p┼éyty** (pole 0ÔÇô1) | ASCII (┬ž10) |
| `0xCE`/`0xCF` | Nazwa p┼éyty (Custom file; CF = miga = aktualnie grana) | po klawiszu LIST |
| `0xD2` | **CD-TEXT: nazwa utworu w trybie CD** | ASCII (┬ž10) ÔÇö widziane w naszym logu |
| `0xD5` | Identyfikator p┼éyty (przy zmianie w trakcie grania) | jak `0xC5` |
| `0xD9` | **Nazwa utworu** (pola 2ÔÇô5) | ASCII (┬ž10) |
| `0xDD` | **Nazwa p┼éyty** (pola 2ÔÇô5) | ASCII (┬ž10) |

### 8.3. Klawisze pilota/joysticka (slave Ôćĺ master, dla kontekstu)
`0x10` OFF/SOURCE, `0x12` DISP/LIST, `0x70` VOLÔłĺ/VOL+, `0x71` MUTE, `0x74` SOUND,
`0x7E` PREV/NEXT DISC i **SEEKÔłĺ/SEEK+**, `0x3C` bezpo┼Ťrednie klawisze p┼éyt 1ÔÇô10.
(Warto┼Ťci CMD2: `ÔÇŽ0` = wci┼Ťni─Öty, `ÔÇŽA` = puszczony.)

---

## 9. Przewijanie i sterowanie odtwarzaniem (szczeg├│┼éowo)

To by┼éa jedna z dw├│ch rzeczy, o kt├│re prosi┼ée┼Ť. Poni┼╝ej kompletny obraz.

### 9.1. Model ÔÇ×press / hold / releaseÔÇŁ
Ôťů Klawisze ci─ůg┼ée (przewijanie) dzia┼éaj─ů w schemacie **start Ôćĺ trzymanie Ôćĺ
koniec**:
- **wci┼Ťni─Öcie** SEEK+ / SEEKÔłĺ Ôćĺ master wysy┼éa do zmieniarki `CMD1=0x24`
  (Fast Forward) lub `CMD1=0x25` (Fast Reverse),
- **puszczenie** klawisza Ôćĺ master wysy┼éa **broadcast `CMD1=0x08, CMD2=0x00`**
  (Key off / Cancel) ÔÇö to **sygna┼é zako┼äczenia** przewijania.

Innymi s┼éowy: przewijanie trwa od `0x24`/`0x25` a┼╝ do `0x08`. (Potwierdzone w
tablicy Mictronics: ÔÇ×start fast forwarding, end with broadcast cmd 0x08ÔÇŁ.)

### 9.2. Co robi zmieniarka (emulator)
Ôťů Na podstawie kodu emulator├│w:
- po `0x24` Ôćĺ zacznij przewija─ç w prz├│d (w emulatorze MP3: skok pozycji do
  przodu / szybkie odtwarzanie),
- po `0x25` Ôćĺ przewijaj w ty┼é,
- ustaw **status = `0x21` (Seeking)** zwracany przy time-pollach,
- po broadcast `0x08` Ôćĺ **zatrzymaj** przewijanie, wr├│─ç do `status = 0x00`
  (Playing), zaktualizuj i wy┼Ťlij now─ů pozycj─Ö (`0x90` / `0xC0`).

### 9.3. Sygnalizacja ÔÇ×seekingÔÇŁ na wy┼Ťwietlaczu
­čöÂ W trybie pokazywania na ekranie master/zmieniarka u┼╝ywaj─ů pola `0xC0`/`0xC1`
do wy┼Ťwietlenia stanu przewijania (np. ÔÇ×--.--ÔÇŁ lub migaj─ůcy ÔÇ×LOADÔÇŁ). Emulator
STM32 wykrywa przewijanie m.in. po `0xC1` z `CMD2=0x2D` (`'-'`).

### 9.4. Pozosta┼ée sterowanie (mapowanie 1:1)
| Klawisz radia | Komenda do zmieniarki | Reakcja emulatora |
|---|:--:|---|
| Ôľ║ PLAY | `0x20` | start grania, wy┼Ťlij pozycj─Ö (`0xC0`) |
| Ôëź SEEK+ (hold) | `0x24` ÔÇŽ `0x08` | przewijaj w prz├│d, status `0x21` |
| Ôë¬ SEEKÔłĺ (hold) | `0x25` ÔÇŽ `0x08` | przewijaj w ty┼é, status `0x21` |
| ÔĆş nast─Öpny utw├│r | `0x26` | track++ |
| ÔĆ« poprzedni utw├│r | `0x27` | trackÔłĺÔłĺ |
| DISC+ | `0x28` | disc++ , status `0x40`Ôćĺ`0x20` |
| DISCÔłĺ | `0x29` | discÔłĺÔłĺ |
| wyb├│r p┼éyty 1ÔÇô10 | `0xB0` (CMD2=disc, D1=track) | ustaw p┼éyt─Ö/utw├│r, status `0x40` |
| REPEAT/SHUF/INTRO | `0x34/0x35/0x36` | prze┼é─ůcz tryb, wy┼Ťlij `0x94` (ikony) |

> **Uwaga o adresach komend:** w naszym logu komendy transportu przychodzi┼éy od
> nadawcy **TAD=`0x11` (klawiatura)**, np. `31 11 28 00 6A` = ÔÇ×Next CDÔÇŁ do
> zmieniarki `0x31`. Emulator powinien reagowa─ç na dan─ů komend─Ö **niezale┼╝nie od
> tego, czy TAD to `0x10` (master) czy `0x11` (keypad)** ÔÇö liczy si─Ö RAD = m├│j
> adres i CMD1.
---

## 10. CD-TEXT i nazwy p┼éyt/utwor├│w (szczeg├│┼éowo)

Druga rzecz, o kt├│r─ů prosi┼ée┼Ť. Mechanizm jest **ÔÇ×┼╝─ůdanie pola Ôćĺ odpowied┼║ z
fragmentemÔÇŁ**, zorganizowany w **pola po znaki**.

### 10.1. Kto inicjuje
Ôťů To **wy┼Ťwietlacz/master ┼╝─ůda** kolejnych p├│l tekstu komend─ů **`CMD1=0x84`**:
| `0x84` CMD2 | Co ┼╝─ůdane | Parametr |
|:--:|---|---|
| `0xD9` | **nazwa utworu** | nast─Öpny bajt (D1) = **numer pola** (0ÔÇô5) |
| `0xDD` | **nazwa p┼éyty** | D1 = numer pola (0ÔÇô5) |
| `0x95` | info magazynka | ÔÇö |
| `0x97` | czas ca┼ék. i liczba utwor├│w p┼éyty | ÔÇö |

Zmieniarka kolejkuje odpowied┼║ i wysy┼éa j─ů po najbli┼╝szym `0x01 0x13` (po
slave-break).

### 10.2. Klasyczne nazwy: 8 znak├│w na pole (`0xC9/0xD9`, `0xCD/0xDD`)
Ôťů Z kodu emulatora STM32 ÔÇö nazwy dzielone s─ů na **pola po 8 znak├│w**
(pola 0ÔÇô5 Ôçĺ do 48 znak├│w). Wyb├│r komendy zale┼╝y od numeru pola:

| Zawarto┼Ť─ç | Pole 0ÔÇô1 | Pola 2ÔÇô5 |
|---|:--:|:--:|
| **Nazwa utworu** | `CMD1=0xC9` | `CMD1=0xD9` |
| **Nazwa p┼éyty** | `CMD1=0xCD` | `CMD1=0xDD` |

Budowa ramki nazwy (long word, 8 znak├│w ASCII):
```
{ 0x70(display), ownaddr, CMD1, c0, c1, c2, c3, c4, c5, c6, c7, 0x00, pole }
   RAD          TAD       CMD1  CMD2 D1 D2 D3 D4 D2_1 D2_2 D2_3 D2_4 D2_5
```
ÔÇö czyli **8 znak├│w** trafia w `CMD2, D1, D2, D3, D4, D2_1, D2_2, D2_3`, a
`D2_4=0x00`, `D2_5 = numer pola` (offset = pole ├Ś 8). Logika p├│l w emulatorze:
pole 0 (`0x0E`) Ôćĺ po wys┼éaniu ustaw pole 1 (`0x1E`) i wy┼Ťlij ponownie; pola 2ÔÇô5
liczone wprost; numer > 5 ko┼äczy przesy┼éanie (`0x0E`).

### 10.3. CD-TEXT `0xD2` (wariant z naszego logu) Ôťů
W sniffie CDX-M670 Ôćö CDX-805 zmieniarka u┼╝ywa┼éa komendy **`0xD2`** (ÔÇ×CD-Text
track name in CD modeÔÇŁ). Tu **pierwszy znak siedzi w `CMD2`**, a ramka niesie
**6 znak├│w**:
```
70 31 D2 4B BE 61 70 69 74 61 02 00 F1 10 D0 0
RAD=70 TAD=31 CMD1=D2
 CMD2=4B='K'
 Parity1=BE
 D1=61='a' D2=70='p' D3=69='i' D4=74='t'
 D2_1=61='a'        -> znaki: "Kapita"
 D2_2=02  (licznik/kontynuacja)
 D2_3=00
 D2_4=F1  (numer p┼éyty: F1 = p┼éyta 1)
 D2_5=10  (typ pola tekstu)
 Parity2=D0
```
­čöÂ Pola pomocnicze wg Mictronics dla `0xD2`: `D2_2` w polu 0 Ôëł `0x04`,
`D2_3` = licznik p├│l, `D2_4` Ôëł `0xF6` (u nas `F1ÔÇŽF4` = numer p┼éyty). Warto┼Ťci
bywaj─ů r├│┼╝ne mi─Ödzy modelami ÔÇö istotne jest, ┼╝e **znaki zaczynaj─ů si─Ö od `CMD2`**.

### 10.4. Tekst odczytany z naszego logu (poprawnie) Ôťů
Po prawid┼éowym dekodowaniu (pierwszy znak = `CMD2`):
| P┼éyta (D2_4) | Fragmenty (kolejne `0xD2`) | Z┼éo┼╝ony tekst |
|:--:|---|---|
| `F1` | `Kapita` + `nskie tÔÇŽ` | **ÔÇ×Kapita[┼ä]skie tÔÇŽÔÇŁ** |
| `F2` | `Do zak` + `ochania` | **ÔÇ×Do zakochaniaÔÇŁ** |
| `F3` | `Mary A` + `nnÔÇŽ` | **ÔÇ×Mary AnnÔÇŽÔÇŁ** (­čöÂ) |
| `F4` | `Starus` + `zek ┼ÜwiÔÇŽ` | **ÔÇ×Staruszek ┼Üwi[─Öty]ÔÇŽÔÇŁ** (­čöÂ) |

(W pierwszej wersji dokumentu te fragmenty by┼éy b┼é─Ödnie zdekodowane, bo `CMD2`
potraktowano jako offset, a nie jako pierwszy znak ÔÇö teraz poprawione.)

### 10.5. Jak emulator ma podawa─ç w┼éasny CD-TEXT
Ôťů Wzorzec dzia┼éania:
1. Trzymaj nazwy jako bufory znak├│w (np. nazwa utworu = nazwa pliku MP3).
2. Po starcie/zmianie p┼éyty zr├│b **slave-break**, by zg┼éosi─ç dost─Öpno┼Ť─ç tekstu;
   w `0x9C`/`0xC5` ustaw flag─Ö ÔÇ×nazwa dost─ÖpnaÔÇŁ (D4_hi = `C`).
3. Na `0x84 0xD9` (┼╝─ůdanie pola nazwy utworu, numer pola w D1) ode┼Ťlij pole:
   - pole 0ÔÇô1 Ôćĺ `0xC9`, pola 2ÔÇô5 Ôćĺ `0xD9` (8 znak├│w, offset = pole├Ś8),
   - albo u┼╝yj wariantu `0xD2` (6 znak├│w od `CMD2`), je┼Ťli celujesz w zachowanie
     jak CDX-805.
4. Analogicznie `0x84 0xDD` Ôćĺ nazwa p┼éyty (`0xCD`/`0xDD`).
5. Gdy numer pola > 5 lub tekst si─Ö sko┼äczy┼é ÔÇö przesta┼ä wysy┼éa─ç kolejne pola.

> **Kodowanie znak├│w:** ASCII 7-bit (w logu m.in. polskie wyrazy zapisane bez
> diakrytyk├│w lub z podmian─ů ÔÇö `Kapitanskie`, `Swiety`). Dla bezpiecze┼ästwa
> ogranicz wysy┼éany tekst do ASCII drukowalnego (0x20ÔÇô0x7E).

---

## 11. Status odtwarzania i pozycja (0x90 / 0xC0)

### 11.1. JAK DZIA┼üA ODLICZANIE CZASU UTWORU Ôťů
**Najwa┼╝niejszy wniosek: zegar prowadzi ZMIENIARKA, a nie radio.** Radio tylko
wy┼Ťwietla liczb─Ö, kt├│r─ů dostaje. To zmieniarka utrzymuje up┼éywaj─ůcy czas
bie┼╝─ůcego utworu i co sekund─Ö wypycha now─ů warto┼Ť─ç na magistral─Ö.

**Pe┼éna sekwencja (zweryfikowana w logu, znaczniki czasu hosta):**
```
18 10 01 15 3E 0                  ; master: "kto chce m├│wi─ç?" (broadcast 0x15)
31 10 01 13 55 0                  ; master daje ZMIENIARCE g┼éos (request poll 0x13)
70 31 90 30 61 F1 F0 03 8B D0 0   ; zmieniarka -> wy┼Ťwietlacz 0x70: czas = 03 s
... (~1 s p├│┼║niej) ...
70 31 90 30 61 F1 F0 04 8A D0 0   ; czas = 04 s
70 31 90 30 61 F1 F0 05 8A D1 0   ; czas = 05 s
70 31 90 30 61 F1 F0 06 8A D2 0   ; czas = 06 s
```
Pomiar odst─Öp├│w mi─Ödzy tymi ramkami: ~0,97ÔÇô1,02 s Ôćĺ **inkrement o 1 sekund─Ö**.

**Krok po kroku, jak czas trafia na ekran:**
1. Zmieniarka odlicza czas w┼éasnym zegarem 1 Hz (ma sta┼éy takt ~1 s).
2. Gdy ma now─ů warto┼Ť─ç, sygnalizuje ch─Ö─ç nadania przez **slave-break** (┬ž2.2).
3. Master pyta rozg┼éoszeniowo `0x01 0x15` (ÔÇ×kto chce m├│wi─ç?ÔÇŁ).
4. Master udziela g┼éosu zmieniarce: `RAD=m├│j_adres, 0x01 0x13`.
5. Zmieniarka wysy┼éa **middle word `0x90`** na adres wy┼Ťwietlacza `0x70`
   z now─ů liczb─ů sekund.
6. Radio od┼Ťwie┼╝a wskazanie zegara. **Radio samo nic nie liczy** ÔÇö je┼Ťli
   zmieniarka przestanie wysy┼éa─ç `0x90`, czas na ekranie zamarznie.

### 11.2. Budowa ramki czasu `0x90` (realny CDX-805, CMD2=0x30) Ôťů
```
70 31 90 30 61 F1 F0 03 8B D0 0
ÔööÔöÇÔöČÔöÇ ÔöÇÔöČÔöÇ ÔöÇÔöČ ÔöÇÔöČ ÔöÇÔöČ ÔöÇÔöČ ÔöÇÔöČ ÔöÇÔöČ ÔöÇÔöČ ÔöÇÔöČ  Ôöö bajt ko┼äca
 RAD  Ôöé  CMD1Ôöé  Ôöé  D1 D2 D3 D4 Parity2
 (70  TAD   CMD2 Parity1
 disp)(31)        =61
```
| Pole | Warto┼Ť─ç | Znaczenie (Ôťů potwierdzone / ­čöÂ wywnioskowane) |
|:--:|:--:|---|
| RAD | `70` | odbiorca = grupa Display (na ekran) |
| TAD | `31` | nadawca = zmieniarka |
| CMD1 | `90` | ÔÇ×pozycja/ustawienia odtwarzaniaÔÇŁ |
| CMD2 | `30` | podtyp ramki (lekki ÔÇ×tikÔÇŁ czasu; wariant CDX-805) ­čöÂ |
| Parity1 | `61` | `70+31+90+30` Ôťö |
| **D1** | `F1` | **numer p┼éyty** ÔÇö `F`= obecna, low nibble = nr (F1=1 ÔÇŽ F4=4) Ôťů |
| **D2** | `F0` | znacznik/ÔÇ×minuty=0ÔÇŁ ÔÇö w tym przechwycie sta┼éy (utw├│r gra┼é <1 min) ­čöÂ |
| **D3** | `01..06` | **UP┼üYWAJ─äCE SEKUNDY ÔÇö ro┼Ťnie o 1 co ~1 s** Ôťů |
| **D4** | `88` | bajt stanu/sterowania wy┼Ťwietlaniem ­čöÂ |
| Parity2 | `CBÔÇŽD2` | `Parity1 + D1+D2+D3+D4` Ôťö |

> **Uwaga o minutach:** w tym przechwyceniu utw├│r gra┼é tylko kilka sekund, wi─Öc
> minuty nie zd─ů┼╝y┼éy si─Ö zmieni─ç i pozycja D2 (`F0`) nie zosta┼éa zweryfikowana
> jako licznik minut. Wg generycznej tabeli Mictronics dla `0x90` minuty s─ů w D2,
> sekundy w D3, a numer p┼éyty w D4_hi ÔÇö ale **realny CDX-805 (CMD2=0x30)** trzyma
> sekundy w D3 i numer p┼éyty w D1. Dlatego dla pewnego liczenia minut bezpiecznie
> jest dodatkowo wysy┼éa─ç pe┼ény status `0xC0` / `0x97` (patrz ┬ž11.4).

### 11.3. Wzorzec z emulatora AVR (dla por├│wnania) Ôä╣´ŞĆ
Emulator Beckera u┼╝ywa wariantu z `CMD2=0x50` i jawnymi min/sek:
```c
#define DISC_MSG {0x10, ownaddr, 0x90, 0x50, tracknbr, min, sec, (disc-0x10)*16}
//  CMD2=0x50  D1=utw├│r  D2=min  D3=sek  D4_hi=p┼éyta
```
Czyli r├│┼╝ne modele radia/zmieniarki akceptuj─ů dwa uk┼éady `0x90`:
`CMD2=0x30` (lekki tik sekund, jak CDX-805) oraz `CMD2=0x50` (pe┼ény utw├│r/min/sek).

### 11.4. Co musi robi─ç emulator, by czas ÔÇ×chodzi┼éÔÇŁ Ôťů
1. Po wej┼Ťciu w stan **Playing (0x00)** uruchom **timer 1 Hz**.
2. Co sekund─Ö: `sek++` (przy 60 Ôćĺ `sek=0; min++`); aktualizuj te┼╝ numer utworu.
3. Zg┼éo┼Ť ch─Ö─ç nadania (**slave-break**) i na grant `0x01 0x13` wy┼Ťlij `0x90`
   z nowym czasem na adres `0x70`.
4. Format minimalny zgodny z CDX-805: `70 <addr> 90 30 <p1> <Fdisc> F0 <sek> <st>`
   (z poprawnymi sumami). Dla pe┼énej zgodno┼Ťci dorzucaj okresowo `0xC0`/`0x97`
   z liczb─ů utwor├│w, minutami i sekundami.
5. W **pauzie/seeku** przesta┼ä inkrementowa─ç (lub wysy┼éaj `--.--` zgodnie z ┬ž9.3);
   po wznowieniu wr├│─ç do inkrementacji.
6. Liczby czasu podawaj tak, jak oczekuje radio ÔÇö w praktyce **BCD** (np. 59 s =
   `0x59`); poni┼╝ej 10 hex i BCD s─ů nierozr├│┼╝nialne, wi─Öc bezpiecznie u┼╝ywa─ç BCD.

### 11.5. `0xC0` ÔÇö status play / pole na ekranie (long word) Ôťů
Niesie komplet: nr p┼éyty, liczb─Ö utwor├│w, czas, sterowanie ekranem. W trybie
MD/CD (wg Mictronics): `D2_2 = liczba utwor├│w, D2_3 = minuty, D2_4 = sekundy,
D2_5_lo = numer p┼éyty, D2_5_hi = sterowanie wy┼Ťwietlaniem`. Po `0xC0` zwykle
nast─Öpuje ramka z **nazw─ů p┼éyty** (`0xCD`).

W emulatorze AVR ÔÇ×seek/pozycja startowaÔÇŁ to:
```c
#define SEEK_MSG {0x70, ownaddr, 0xC0, 0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x18}
```
wysy┼éane zaraz po `PLAY (0x20)`.

W naszym logu warianty `0xC0` (numer p┼éyty `F1ÔÇŽF4`, pola `F0 00` = czas wa┼╝ny,
`FF FF` = skan/odczyt TOC):
```
70 31 C0 40 A1 00 00 00 00 00 F1 F0 00 11 93 0   ; p┼éyta 1, gra
77 31 C0 20 88 00 00 00 00 30 F2 FF FF 88 30 0   ; p┼éyta 2, skan (FFFF)
```

### 11.6. Numer p┼éyty
Ôťů Kodowany jako g├│rny nibble bajtu ÔÇ×discÔÇŁ = `F` (obecna), dolny = numer:
`F1`=1, `F2`=2, `F3`=3, `F4`=4. (St─ůd w skanie magazynka po starcie wida─ç
sekwencj─Ö `F1ÔćĺF2ÔćĺF3ÔćĺF4` = czytanie TOC i CD-TEXT czterech p┼éyt.)
---

## 12. Wytyczne dla emulatora zmieniarki

### 12.1. Warstwa sprz─Ötowa
- Trzy linie: **BUS ON** (wej┼Ťcie ÔÇö ÔÇ×radio w┼é─ůczoneÔÇŁ), **CLOCK** (wej┼Ťcie ÔÇö zegar
  od mastera), **DATA** (dwukierunkowa, open-collector).
- Nadawanie = ┼Ťci─ůganie DATA do masy w takt zegara mastera (urz─ůdzenie jest
  **slave SPI-podobny**: master daje zegar, my wystawiamy bity ÔÇö emulatory u┼╝ywaj─ů
  sprz─Ötowego SPI w trybie slave).
- Obs┼éu┼╝ **wake-up** (wymuszenie DATA high budzi mastera) i **BUS ON** jako sygna┼é
  zasilania.

### 12.2. Warstwa ramki (obowi─ůzkowe)
1. Odbi├│r: zlicz d┼éugo┼Ť─ç po **CMD1** (`<0x80`=6, `<0xC0`=11, inaczej 16 bajt├│w).
2. Sprawd┼║ **obie** sumy (Parity1, Parity2) + ko┼äcowe `0`; odrzucaj b┼é─Ödne.
3. Reaguj, gdy **RAD == m├│j przydzielony adres** lub `RAD == 0x18` (broadcast)
   albo `RAD == (moja_grupa | 0)` (broadcast grupowy).
4. Buduj ramki z poprawnymi Parity1/Parity2 (┬ž4.2).
5. Odpowiadaj szybko (rz─Ödu ms; w logu ~6 ms).

### 12.3. DYNAMICZNY adres ÔÇö krok po kroku
1. Start: `ownaddr = 0x30` (grupa CD, brak ID).
2. Na broadcast **`0x01 0x02` (Anyone)** Ôćĺ wy┼Ťlij `CD_DEVICE_MSG` (`0x8CÔÇŽ`).
3. Na **Appoint** (`CMD1=0x02`, RAD w mojej grupie `0x3X`) Ôćĺ `ownaddr = RAD`,
   potwierd┼║ `CD_DEVICE_MSG`.
4. Na `0x01 0x00` (reset) Ôćĺ `ownaddr = 0x30` i czekaj na ponowny Anyone.
5. Od tej chwili u┼╝ywaj `ownaddr` jako adresu ┼║r├│d┼éowego (TAD) i filtra RAD.

> **Nie zak┼éadaj `0x31` na sztywno.** To radio decyduje o dolnym nibble adresu.

### 12.4. Maszyna stan├│w (minimum, by ÔÇ×gra┼éoÔÇŁ)
| Odebrane (RAD=m├│j / broadcast) | Reakcja |
|---|---|
| `0x01 0x02` Anyone | wy┼Ťlij device info `0x8C` |
| `0x02` Appoint | zapami─Ötaj adres, potwierd┼║ `0x8C` |
| `0x01 0x12` time-poll | ode┼Ťlij `0x00 status` (PONG) |
| `0x01 0x13` request-poll | wy┼Ťlij zakolejkowany blok (status `0xC0`, info `0x97/0x95`, nazwa `0xC9/0xCD/0xD2`, czas `0x90`) |
| `0x20` PLAY | status=`0x00`, wy┼Ťlij pozycj─Ö `0xC0` |
| `0x24`/`0x25` FF/REW | status=`0x21`, przewijaj; zako┼äcz na broadcast `0x08` |
| `0x26/0x27` next/prev track | zmie┼ä utw├│r, wy┼Ťlij `0x90` |
| `0x28/0x29` next/prev CD | zmie┼ä p┼éyt─Ö, status `0x40`Ôćĺ`0x20`, wy┼Ťlij `0xC0`+nazwy |
| `0xB0` wyb├│r p┼éyty | ustaw p┼éyt─Ö/utw├│r (CMD2=disc, D1=track) |
| `0x84 0xD9/0xDD` ┼╝─ůdanie tekstu | ode┼Ťlij pole nazwy utworu/p┼éyty |
| `0x84 0x95/0x97` ┼╝─ůdanie info | ode┼Ťlij map─Ö p┼éyt / czas i liczb─Ö utwor├│w |
| broadcast `0xF0 SRC` (nie m├│j) | przejd┼║ w `0x80` (idle ÔÇö zdeselekcjonowano) |
| broadcast `0x87 0x00` | power-off Ôćĺ `0x80` (idle) |

### 12.5. Dane do wygenerowania
- **Device info `0x8C`** (grupa CD + liczba slot├│w) ÔÇö patrz ┬ž6.1.
- **Status `0x00`** (PONG) i mapa stan├│w (┬ž7.1).
- **Pozycja `0x90`** z numerem utworu/p┼éyty i **sekundami rosn─ůcymi co 1 s**.
- **`0xC0`** status play + `0x97` (czas/utwory) + `0x95` (mapa p┼éyt).
- **Nazwy `0xC9/0xCD`** lub `0xD2` (CD-TEXT) ÔÇö pola po 8 (lub 6) znak├│w.

### 12.6. Pu┼éapki
- D┼éugo┼Ť─ç ramki wynika z **CMD1**, nie z adresu.
- **Parity2** liczy si─Ö od Parity1 (nie licz Parity1 podw├│jnie i nie wliczaj
  ko┼äcowego `0`).
- Adres jest **dynamiczny** ÔÇö obs┼éu┼╝ Anyone/Appoint/Reset.
- Numer p┼éyty to `F|nr` (`F1`=1), nie `0x01`.
- CD-TEXT: pierwszy znak bywa w **CMD2** (wariant `0xD2`).
- Dane ÔÇ×na ekranÔÇŁ wysy┼éaj na **`0x70`** (display), status/ACK na **`0x10`**
  (master) ÔÇö z TAD = Tw├│j przydzielony adres.
- Ruch mi─Ödzy `0x10`Ôćö`0x18`Ôćö`0x70`Ôćö`0x91` to w du┼╝ej cz─Ö┼Ťci wewn─Ötrzne sprawy
  g┼éowicy (wy┼Ťwietlacz, DSP, tuner) ÔÇö emulator je ignoruje.

---

## Za┼é─ůcznik A: format logu snifera

```
[hh:mm:ss.mmm] t=<us> dt=<przerwa_us> bus=<0/1> : B0 B1 ... Bn 00
```
- `t=` ÔÇö czas wewn─Ötrzny (┬Ás), `dt=` ÔÇö odst─Öp od poprzedniej aktywno┼Ťci (┬Ás),
- `bus=` ÔÇö stan zasilania magistrali, po dwukropku bajty HEX,
- ko┼äcowe `00` = bajt ko┼äcz─ůcy word (nie wchodzi do sum),
- linie `BUS=0/1` = zmiana zasilania, `minBitGap=ÔÇŽus` = auto-pomiar,
- `!` przy bajcie = bajt Ôëá 8 bit├│w (b┼é─ůd),
- samodzielna linia `: 00` = pojedynczy bajt potwierdzenia/wype┼énienia.

## Za┼é─ůcznik B: przyk┼éady z pe┼énym dekodowaniem

```
; --- POLL (short) ---
31 10 01 12 54 0
RAD=31(zmieniarka) TAD=10(master) CMD1=01 CMD2=12(time-poll)
Parity1 = 31+10+01+12 = 54 Ôťö

; --- STATUS / PONG (short) ---
10 31 00 80 C1 0
RAD=10(master) TAD=31 CMD1=00(status) CMD2=80(idle)
Parity1 = 10+31+00+80 = C1 Ôťö

; --- POZYCJA 0x90 (middle) ---
70 31 90 30 61 F1 F0 03 8B D0 0
RAD=70(display) TAD=31 CMD1=90 CMD2=30 Parity1=61
D1=F1 D2=F0 D3=03(sek) D4=8B   Parity2 = 61+F1+F0+03+8B = D0 Ôťö

; --- CD-TEXT 0xD2 (long) ---
70 31 D2 4B BE 61 70 69 74 61 02 00 F1 10 D0 0
RAD=70 TAD=31 CMD1=D2 CMD2='K' Parity1=BE
znaki: K a p i t a = "Kapita"  ; p┼éyta F1
Parity2 = BE+61+70+69+74+61+02+00+F1+10 = D0 Ôťö

; --- STATUS PLAY 0xC0 (long) ---
70 31 C0 40 A1 00 00 00 00 00 F1 F0 00 11 93 0
RAD=70 TAD=31 CMD1=C0 CMD2=40 Parity1=A1
...(pola statusu, p┼éyta F1)...  Parity2 = 93 Ôťö

; --- NEXT CD (short, od klawiatury) ---
31 11 28 00 6A 0
RAD=31(zmieniarka) TAD=11(keypad) CMD1=28(Next CD)
Parity1 = 31+11+28+00 = 6A Ôťö
```

---

## ┼╣r├│d┼éa

Materia┼éy pobrane lokalnie do `./zrodla/`:
- **Mictronics ÔÇö ÔÇ×Inside Sony UnilinkÔÇŁ** (opis magistrali, ramki, adresy):
  <https://www.mictronics.de/posts/Inside-Sony-Unilink/>
- **Mictronics ÔÇö tablica komend** (`command.htm`):
  <https://www.mictronics.de/command.htm>
- **GNUnilink** (emulator zmieniarki na PIC, model master-poll):
  <https://gnunilink.sourceforge.net/> ; wiki: <http://sophana.free.fr/pmwiki2/index.php5/GNUnilink/Faq>
- **Becker/AVR Unilink** (Michael Wolf) ÔÇö `unilink.c`/`unilink.h` (procedury
  appoint, status, slave-break, nazwy) ÔÇö GPL.
- **STM32 ÔÇ×Alfa-166 Unilink CD emulatorÔÇŁ** (deividAlfa) ÔÇö nowszy emulator z
  CD-TEXT/USB; zawiera te┼╝ kopie dokumentacji Mictronics i logi:
  <https://github.com/deividAlfa/Alfa-166-Unilink-CD-emulator>
- **Sony** ÔÇö opis z┼é─ůcza BUS / kabla UniLink:
  <https://www.sony.com/electronics/support/articles/00032155>

*Informacje o protokole pochodz─ů z in┼╝ynierii wstecznej prowadzonej przez
spo┼éeczno┼Ť─ç (Mictronics, GNUnilink, autorzy emulator├│w) i nie s─ů oficjaln─ů
dokumentacj─ů Sony ÔÇö u┼╝ywa─ç na w┼éasn─ů odpowiedzialno┼Ť─ç. Tre┼Ťci ze ┼║r├│de┼é
zewn─Ötrznych sparafrazowano dla zgodno┼Ťci z ograniczeniami licencyjnymi. Sony i
Unilink to zastrze┼╝one znaki towarowe Sony Corporation.*

---

## Za┼é─ůcznik C: weryfikacja typ├│w ramek na realnym logu CDX-805

Ôťů Poni┼╝ej **realne ramki** zmieniarki CDX-805 z naszego sniffu, zmapowane na
definicje Mictronics. To gotowy materia┼é referencyjny dla emulatora (wszystkie
sumy kontrolne si─Ö zgadzaj─ů).

### C.1. Handshake przydzia┼éu adresu (dow├│d na DYNAMICZNY adres)
Master rozsy┼éa **Appoint** (`CMD1=0x02`) nadaj─ůc adresy kolejnym urz─ůdzeniom:
```
3B 10 02 11 5E 0     ; do urz─ůdzenia grupy CD (ID B)
DB 10 02 12 FF 0     ; do zmieniarki MD (grupa D, ID B)
71 10 02 12 95 0     ; do urz─ůdzenia grupy Display (ID 1)
31 10 02 14 57 0     ; do NASZEJ zmieniarki -> przydzielony adres 0x31
```
Ôćĺ To radio przypisa┼éo `0x31`. Emulator musi przyj─ů─ç **dowolny** ID z grupy `0x3X`.

### C.2. Device info `0x8C` (przed i po przydziale ID)
```
10 30 8C D0 9C 05 A8 1F A3 0B 0   ; TAD=30 (jeszcze bez ID) -> info do mastera
10 31 8C D0 9D 04 A8 1F A3 0B 0   ; TAD=31 (po przydziale)  -> potwierdzenie
  RAD=10(master) CMD1=8C CMD2=D0  Parity1: 10+30+8C+D0=9C Ôťö / 10+31+8C+D0=9D Ôťö
  D1..D4 = 05/04 A8 1F A3  -> ┼éa┼äcuch mo┼╝liwo┼Ťci CDX-805 (typ/sloty/Custom file)
```
> To jest realny string ÔÇ×jestem zmieniark─ů CDÔÇŁ konkretnie z CDX-805 ÔÇö mo┼╝na go
> u┼╝y─ç w emulatorze 1:1 (z w┼éasnym TAD = przydzielony adres).

### C.3. Magazynek / mapa p┼éyt
```
70 31 8E C0 EF 00 00 00 80 6F 0   ; 0x8E info magazynka (CMD2: C0/80/F0 = stany)
70 31 95 08 3E 00 00 00 8A C8 0   ; 0x95 mapa obecnych p┼éyt (po skanie magazynka)
90 31 9C 00 5D 00 00 00 88 E5 0   ; 0x9C zmiana p┼éyty -> RAD 0x90; D4_hi=8=brak nazwy
```

### C.4. Identyfikator p┼éyty (disc ID) ÔÇö `0xC5` / `0xD5`
```
10 31 C5 A2 A8 24 77 52 F0 00 00 00 00 88 0D 0  ; po skanie/zmianie magazynka
10 31 D5 A2 B8 24 77 52 F1 00 00 00 00 88 1E 0  ; przy zmianie p┼éyty w trakcie grania
  CMD1=C5/D5  CMD2=A2 (high 'A' sta┼éy, low=1/100 s)
  D1=24 (liczba utwor├│w)  D4=F0 (sta┼éy znacznik wg Mictronics Ôťö)
  reszta = unikatowy nr p┼éyty liczony z danych p┼éyty (do dopasowania CD-TEXT)
```
Radio u┼╝ywa disc ID, by **rozpozna─ç p┼éyt─Ö** i ewentualnie wczyta─ç dla niej
zapami─Ötan─ů nazw─Ö/CD-TEXT. Emulator mo┼╝e poda─ç dowolny, ale **sta┼éy dla danej
p┼éyty** identyfikator.

### C.5. Pozycja i status (potwierdzenie licznik├│w)
```
70 31 90 30 61 F1 F0 01 88 CB 0   ; sek=01
70 31 90 30 61 F1 F0 02 8B CF 0   ; sek=02  (inkrement co ~1 s -> zegar utworu)
70 31 C0 40 A1 00 00 00 00 00 F1 F0 00 11 93 0  ; status play, p┼éyta F1
77 31 C0 40 A8 00 00 00 00 00 F1 F0 00 21 AA 0  ; ... (RAD=77, drugi kana┼é display)
```

### C.6. Mapowanie skr├│t├│w do sekcji
| Typ z logu | CMD1 | Sekcja opisu |
|---|:--:|---|
| Appoint / dynamiczny adres | `0x02` | ┬ž5, ┬ž6, C.1 |
| Device info | `0x8C` | ┬ž6.1, C.2 |
| Magazynek / mapa p┼éyt | `0x8E`/`0x95`/`0x9C` | ┬ž8.2, C.3 |
| Disc ID | `0xC5`/`0xD5` | ┬ž8.2, C.4 |
| Pozycja / status | `0x90`/`0xC0` | ┬ž11, C.5 |
| CD-TEXT | `0xD2` (i `0xC9/0xCD/0xD9/0xDD`) | ┬ž10 |
| Przewijanie | `0x24`/`0x25` + `0x08` | ┬ž9 |
