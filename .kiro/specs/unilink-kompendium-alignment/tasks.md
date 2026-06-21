# Implementation Plan

## Overview

Plan przyrostowo uzgadnia emulator zmieniarki CD Sony UniLink (ESP32 / Arduino
C++) z protokołem opisanym w Kompendium. Kolejność jest bezpieczna: najpierw
wydzielenie czystej logiki ramek (`UnilinkFrame`) wraz ze środowiskiem testów
host-side (RapidCheck, natywna kompilacja), bo to fundament nadawania i
walidacji. Następnie podpięcie tej logiki w nadawaniu (`UnilinkBus`), parser po
CMD1, walidacja w `handlePacket`, adresowanie `0x30`, rozdzielenie stanu
mechanizmu (`MechState`) od bajtu statusu, kolejka TX, a na końcu nowe funkcje
(CD-TEXT, magazynek, tryby, `0xB0`, `0x08`, `0x90`/`0xC0`) i dokumentacja
rozbieżności (R12).

Strategia testów jest dwutorowa: 22 właściwości poprawności jako testy
property-based (≥100 iteracji) na czystej logice kompilowanej natywnie na hoście,
oraz testy jednostkowe dyspozytora. Testy na realnym radiu (CDX-M670,
MEX-BT3800u) są opcjonalną weryfikacją końcową i nie blokują zadań kodowych.

Każdy test property-based zawiera komentarz w formacie:
`Feature: unilink-kompendium-alignment, Property {n}: {treść}` oraz odwołanie
`Validates: Requirements X.Y`.

## Tasks

- [x] 1. Środowisko testów host-side i fundament UnilinkFrame
  - [x] 1.1 Skonfigurować środowisko testów property-based
    - Utworzyć katalog `ESP32/Emulator/test/` z natywną konfiguracją budowania (CMake/Makefile) kompilującą czyste moduły bez `Arduino.h`
    - Dodać zależność RapidCheck i runner testów (≥100 iteracji na property)
    - Dodać pojedynczy „smoke test", aby zweryfikować, że pipeline natywnej kompilacji i uruchamiania testów działa
    - _Requirements: 1.1_
  - [x] 1.2 Zaimplementować moduł UnilinkFrame (czysta logika ramek)
    - Utworzyć `UnilinkFrame.h`/`UnilinkFrame.cpp` zależne tylko od `<stdint.h>`/`<stddef.h>`
    - Zaimplementować `sizeFromCmd1`/`lengthFromCmd1` (6/11/16 wg CMD1), `parity1`, `parity2`, `validate` (ValidateResult), `encodeDiscNibble`/`discNibbleToNumber`, `encodeBcd`/`decodeBcd`/`encodeBcdFpad`
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 3.1, 3.2, 3.3, 7.4, 11.1, 11.2_
  - [x]* 1.3 Napisać property test długości ramki z CMD1
    - **Property 3: Długość ramki wyznaczona przez CMD1**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 3: Dla dowolnej wartości CMD1 0x00–0xFF lengthFromCmd1 zwraca 6 (<0x80), 11 (0x80..0xBF), 16 (>=0xC0)`
    - **Validates: Requirements 3.1, 3.2, 3.3**
  - [x]* 1.4 Napisać property test round-trip walidacji nadawanych ramek
    - **Property 1: Round-trip walidacji sum kontrolnych nadawanych ramek**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 1: Ramka zbudowana z policzonymi Parity1/Parity2 i bajtem końcowym 0 przechodzi validate z wynikiem Ok`
    - **Validates: Requirements 2.1, 2.4**
  - [x]* 1.5 Napisać property test odrzucania przekłamanych ramek
    - **Property 2: Walidator odrzuca ramki z przekłamanymi sumami lub bajtem końcowym**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 2: Zmiana bajtu nagłówka/danych/końca daje wynik != Ok (BadParity1/BadParity2/BadEnd)`
    - **Validates: Requirements 2.1, 2.2, 2.3**
  - [x]* 1.6 Napisać property test round-trip kodowania BCD czasu
    - **Property 22: Round-trip kodowania BCD czasu**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 22: Dla 0–59 encodeBcd/decodeBcd są wzajemnie odwrotne, a encodeBcdFpad dla <10 ustawia górny nibble F`
    - **Validates: Requirements 11.1, 11.2**
  - [x]* 1.7 Napisać property test round-trip kodowania numeru płyty F|nr
    - **Property 11: Round-trip kodowania numeru płyty F|nr**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 11: Dla płyty 1–9 encodeDiscNibble daje 0xF1–0xF9, a discNibbleToNumber jest jego odwrotnością`
    - **Validates: Requirements 7.4**

- [x] 2. Refaktor nadawania ramek w UnilinkBus na UnilinkFrame
  - [x] 2.1 Przepiąć nadawanie na jedno źródło parzystości
    - Zmienić `sendShort`/`sendMedium`/`sendLong` w `UnilinkBus.cpp`, aby liczyły sumy przez `UnilinkFrame::parity1`/`parity2` zamiast obliczeń inline
    - Zachować bez zmian wartości czasowe i mechanizm Slave Break (operujemy tylko na zawartości bajtów)
    - _Requirements: 2.1, 2.4_
  - [x]* 2.2 Napisać test jednostkowy zgodności nadawanych ramek
    - Zbudować przykładowe ramki short/middle/long funkcjami nadawczymi i sprawdzić, że `UnilinkFrame::validate` zwraca `Ok`
    - _Requirements: 2.1, 2.4_

- [x] 3. Parser ramek po CMD1 w UnilinkBus
  - [x] 3.1 Zaimplementować wyznaczanie granicy ramki z CMD1
    - Dodać `readFrame(out, maxLen)` w `UnilinkBus`: gdy w buforze ≥3 bajty, odczytać `cmd1` i wyznaczyć `expected = UnilinkFrame::lengthFromCmd1(cmd1)`; udostępnić ramkę po osiągnięciu `expected`
    - Pozostawić wykrywanie ciszy (`READ_SILENCE_US`) wyłącznie jako zabezpieczenie awaryjne (resynchronizacja), bez zmiany wartości czasowych
    - _Requirements: 3.4, 3.5_
  - [x]* 3.2 Napisać property test cięcia strumienia po granicach CMD1
    - **Property 4: Parser tnie strumień dokładnie po granicach wyznaczonych przez CMD1**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 4: Dla dowolnej sekwencji sklejonych poprawnych ramek model parsera wyodrębnia te same ramki wg długości z CMD1`
    - Wydzielić czystą funkcję modelu parsera (bez ISR) do testu host-side
    - **Validates: Requirements 3.4, 3.5**

- [x] 4. Walidator_Ramek w dyspozytorze handlePacket
  - [x] 4.1 Dodać walidację sum na wejściu handlePacket
    - W `UnilinkProtocol::handlePacket` zastąpić sprawdzenie `buf[4]` wywołaniem `UnilinkFrame::validate(buf, len)`; przy wyniku != Ok odrzucić ramkę i zapisać `Diagnostics::recordNote("DROP")`
    - Odrzucona ramka nie zmienia stanu sesji ani zmieniarki
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5_
  - [x]* 4.2 Napisać test jednostkowy rejestracji DROP
    - Podać ramkę middle/long z błędnym Parity2 i sprawdzić, że jest odrzucona oraz odnotowana jako zdarzenie diagnostyczne
    - _Requirements: 2.5_

- [x] 5. Dynamiczne adresowanie (AddressManager) i adres grupowy 0x30
  - [x] 5.1 Zmienić adres startowy w Config.h
    - Wprowadzić `ADDR_GROUP_CD = 0x30` i `ADDR_DEFAULT = ADDR_GROUP_CD`; oznaczyć zmianę komentarzem `// [DEVIATION §5/§6]` (było `0x31`)
    - _Requirements: 4.1, 4.5_
  - [x] 5.2 Zaimplementować logikę AddressManager
    - Wydzielić stan `myAddr`/`allocated` i reguły: start/reset → `0x30`, Anyone (`0x01 0x02`) bez ID → device info `0x8C`, Appoint (`CMD1=0x02`, `RAD & 0xF0 == 0x30`) → adopcja `RAD`, Bus reset (`0x01 0x00`) → `0x30`
    - Używać `myAddr` jako TAD we wszystkich odpowiedziach; zachować kwirki CDX-M670 jako wariant discovery
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5_
  - [x]* 5.3 Napisać property test adopcji ID i powrotu do 0x30
    - **Property 5: Adopcja dowolnego ID z grupy CD i powrót do 0x30**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 5: Dla RAD z 0x31–0x3F po Appoint myAddr=RAD, a po Bus reset myAddr=0x30`
    - **Validates: Requirements 4.3, 4.4**
  - [x]* 5.4 Napisać property test niezmiennika adresu przed przydziałem
    - **Property 6: Niezmiennik adresu przed przydziałem ID**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 6: Dla ciągu zdarzeń bez Appoint TAD pozostaje 0x30 i nie przyjmuje stałego ID`
    - **Validates: Requirements 4.1, 4.5**
  - [x]* 5.5 Napisać test jednostkowy odpowiedzi Anyone i startu
    - Sprawdzić, że bez ID na `0x01 0x02` wysyłane jest device info `0x8C` oraz że adres startowy to `0x30`
    - _Requirements: 4.1, 4.2_

- [x] 6. Rozdzielenie stanu mechanizmu (MechState) od bajtu statusu
  - [x] 6.1 Wprowadzić MechState w CdChanger
    - Zastąpić `enum State` (mieszający stan i bajt statusu) przez `enum class MechState`; przepiąć maszynę stanów `update` i porównania na `MechState`
    - _Requirements: 5.3_
  - [x] 6.2 Dodać mapowanie statusByte i użyć go w odpowiedziach
    - Zaimplementować `UnilinkFrame::statusByte(MechState)` wg §7.1 (Playing→0x00, ChangedCd→0x20, Seeking→0x21, Changing/LoadingTrack→0x40, Idle→0x80, Ejecting→0xC0, Init→0x80)
    - W PONG i ramkach statusu czytać `statusByte(mechState())`; oznaczyć mapowanie `Init→0x80` komentarzem `// [DEVIATION §7.1]`
    - _Requirements: 5.1, 5.2, 5.3, 5.4_
  - [ ]* 6.3 Napisać property test mapowania MechState→bajt statusu
    - **Property 7: Mapowanie stanu mechanizmu na kod statusu zgodny z §7.1**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 7: Dla dowolnego MechState statusByte zwraca kod z {0x00,0x20,0x21,0x40,0x80,0xC0} z poprawnym przypisaniem`
    - **Validates: Requirements 5.1, 5.2, 10.1**
  - [ ]* 6.4 Napisać test jednostkowy sekwencji zmiany płyty
    - Sprawdzić sekwencję statusów `0x40 (Changing) → 0x20 (ChangedCd) → 0x00 (Playing)`
    - _Requirements: 5.3_

- [x] 7. Checkpoint — fundament protokołu
  - Ensure all tests pass, ask the user if questions arise.

- [x] 8. Kolejka TX (TxQueue) i cykl request-poll 0x13
  - [x] 8.1 Zaimplementować strukturę i operacje TxQueue
    - Dodać `TxQueue` (ring o stałym rozmiarze, bez alokacji), `TxItem {priority,len,bytes[16]}`, `enqueue`/`dequeue` wg priorytetu; przy pełnej kolejce porzucać najstarszą ramkę o najniższym priorytecie
    - _Requirements: 1.1_
  - [x] 8.2 Zintegrować kolejkę z cyklem Slave Break / grant 0x13
    - Po zdarzeniu enqueue wystawiać Slave Break; na grant `0x01 0x13` zdejmować jedną ramkę; PONG na time-poll (`0x01 0x12`) wysyłać natychmiast poza kolejką
    - Zachować bez zmian stałe czasowe Slave Break (oznaczone `// [HIGH-RISK]`)
    - _Requirements: 1.1, 1.3_
  - [ ]* 8.3 Napisać test jednostkowy priorytetów i przepełnienia kolejki
    - Sprawdzić kolejność zdejmowania wg priorytetu i porzucanie najniższego priorytetu przy przepełnieniu
    - _Requirements: 1.1_

- [x] 9. Moduł CD-TEXT
  - [x] 9.1 Dodać interfejs nazw w AudioPlayer
    - Zaimplementować `audioGetTrackName(disc,track,out,maxLen)` i `audioGetDiscName(disc,out,maxLen)` w `AudioPlayer.h/.cpp` (źródło: nazwy plików/katalogów)
    - _Requirements: 6.1, 6.2_
  - [x] 9.2 Zaimplementować moduł CdText
    - Utworzyć `CdText.h/.cpp`: `sanitizeAscii` (0x20–0x7E), `buildField8` (pola 8-znakowe, offset field*8), `buildFieldD2` (6 znaków, pierwszy znak w CMD2), `fieldExists`, `reassemble`
    - _Requirements: 6.3, 6.4, 6.5, 6.6, 6.7, 6.8_
  - [x] 9.3 Obsłużyć żądania CD-TEXT w dyspozytorze
    - W `handlePacket`: `0x84 0xD9` (utwór, pole w D1) → `0xC9`/`0xD9`; `0x84 0xDD` (płyta) → `0xCD`/`0xDD`; odpowiedź kolejkowana w `TxQueue`; pole > 5 lub koniec tekstu kończy przesyłanie; wybór wariantu `0xD2` wg kontekstu
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 6.7_
  - [x]* 9.4 Napisać property test round-trip CD-TEXT
    - **Property 8: Round-trip CD-TEXT (podział na pola i ponowne złożenie)**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 8: Dla nazwy ASCII 0x20–0x7E podział na pola (warianty 0xC9/0xD9 oraz 0xD2) i ponowne złożenie odtwarza tę samą nazwę`
    - **Validates: Requirements 6.7, 6.8**
  - [x]* 9.5 Napisać property test wyboru komendy wg numeru pola
    - **Property 9: Wybór komendy CD-TEXT według numeru pola**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 9: Pola 0–1 używają 0xC9 (utwór)/0xCD (płyta), pola 2–5 używają 0xD9/0xDD`
    - **Validates: Requirements 6.3, 6.4**
  - [x]* 9.6 Napisać property test sanityzacji ASCII
    - **Property 10: Sanityzacja do drukowalnego ASCII**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 10: Dla dowolnego wejścia wszystkie wysyłane bajty należą do 0x20–0x7E`
    - **Validates: Requirements 6.6**

- [x] 10. Moduł Magazynka
  - [x] 10.1 Zaimplementować moduł Magazine
    - Utworzyć `Magazine.h/.cpp`: `presenceMap` (bity obecności), `buildDiscInfo` (utwory+czas), `buildDiscId` (deterministyczny, cache'owany per płyta), `discNumberByte` (przez `UnilinkFrame::encodeDiscNibble`)
    - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5_
  - [x] 10.2 Obsłużyć żądania magazynka w dyspozytorze
    - W `handlePacket`: `0x84 0x95` → `0x95` (mapa), `0x84 0x97` → `0x97` (info płyty), skan/zmiana płyty → `0xC5`/`0xD5` (disc ID); odpowiedzi kolejkowane w `TxQueue`
    - _Requirements: 7.1, 7.2, 7.3_
  - [x]* 10.3 Napisać property test mapy obecności płyt
    - **Property 12: Mapa obecności płyt odzwierciedla dostępne płyty**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 12: W mapie 0x95 bit płyty jest ustawiony wtedy i tylko wtedy, gdy płyta jest obecna`
    - **Validates: Requirements 7.1**
  - [x]* 10.4 Napisać property test determinizmu disc ID
    - **Property 13: Determinizm identyfikatora płyty**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 13: Wielokrotne buildDiscId dla tej samej płyty zwraca ten sam identyfikator`
    - **Validates: Requirements 7.5**

- [x] 11. Tryby Repeat/Shuffle/Intro i ramka ikon 0x94
  - [x] 11.1 Dodać stan PlayModes i obsługę komend toggle
    - Wprowadzić `RepeatMode`/`PlayModes` w `CdChanger`; obsłużyć `0x34` (cykl Repeat Off→One→All→Off), `0x35` (toggle Shuffle), `0x36` (toggle Intro)
    - _Requirements: 8.1, 8.2, 8.3_
  - [x] 11.2 Zbudować i wysłać ramkę ikon 0x94
    - Po każdej zmianie trybu zakolejkować w `TxQueue` ramkę `0x94` odzwierciedlającą bieżący stan trybów
    - _Requirements: 8.4_
  - [x] 11.3 Uwzględnić tryb Repeat w nawigacji odtwarzania
    - W wyborze następnego utworu (`serviceAutoAdvance`): `One` powtarza utwór, `All` zawija po ostatniej płycie, `Off` zatrzymuje po ostatnim utworze ostatniej płyty
    - _Requirements: 8.5_
  - [ ]* 11.4 Napisać property test cyklu Repeat i idempotencji toggle
    - **Property 14: Cykl Repeat i idempotencja pary toggle**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 14: Pełny cykl 0x34 wraca do stanu początkowego, a para 0x35 lub para 0x36 przywraca stan początkowy trybu`
    - **Validates: Requirements 8.1, 8.2, 8.3**
  - [ ]* 11.5 Napisać property test round-trip stan↔ramka 0x94
    - **Property 15: Ramka ikon 0x94 odzwierciedla stan trybów (round-trip stan↔ramka)**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 15: Dla dowolnego PlayModes ramka 0x94 zbudowana i zdekodowana daje ten sam stan trybów`
    - **Validates: Requirements 8.4**
  - [ ]* 11.6 Napisać property test wyboru następnego utworu wg Repeat
    - **Property 16: Tryb Repeat steruje wyborem następnego utworu**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 16: One zwraca ten sam utwór, All zawija kolejność, Off zatrzymuje po ostatnim utworze ostatniej płyty`
    - **Validates: Requirements 8.5**

- [x] 12. Bezpośredni wybór płyty 0xB0
  - [x] 12.1 Obsłużyć komendę 0xB0
    - W `handlePacket`: `CMD1=0xB0`, `CMD2=disc`, `D1=track`; walidacja zakresu przez `Magazine`/`AudioPlayer`; przy poprawnym zakresie ustawić bieżącą płytę/utwór, wejść w przejście i zakolejkować zaktualizowany status; poza zakresem zignorować bez zmiany stanu
    - _Requirements: 9.1, 9.2, 9.3, 9.4_
  - [ ]* 12.2 Napisać property test respektowania zakresu przez 0xB0
    - **Property 17: Bezpośredni wybór płyty 0xB0 respektuje zakres**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 17: Dla wartości w zakresie płyta/utwór = (CMD2,D1); poza zakresem płyta/utwór bez zmian`
    - **Validates: Requirements 9.1, 9.2, 9.3**

- [x] 13. Przewijanie i zakończenie broadcastem 0x08
  - [x] 13.1 Obsłużyć start przewijania i broadcast 0x08
    - `0x24`/`0x25` → start FF/REW, `MechState::Seeking`, status `0x21`; nowy handler broadcastu `0x08 0x00` (RAD=`0x18`) → powrót do `Playing` (`0x00`) i zakolejkowanie nowej pozycji `0x90`/`0xC0`
    - Zachować skanowanie zatrzaskowe (`seekScanDir`/`serviceSeekRepeat`) jako fallback z `// [DEVIATION §9]`; model `0x08` jako podstawowy
    - _Requirements: 10.1, 10.2, 10.3, 10.4_
  - [ ]* 13.2 Napisać property test round-trip seek→0x08→playing
    - **Property 18: Round-trip stanu przewijania (seek → 0x08 → playing)**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 18: Po starcie 0x24/0x25 odebranie 0x08 0x00 przywraca statusByte == 0x00`
    - **Validates: Requirements 10.2**

- [x] 14. Ramka pozycji 0x90 (1 Hz) i pełny status 0xC0
  - [x] 14.1 Zbudować ramkę 0x90 i harmonogram 1 Hz
    - Budowa `0x90` na RAD `0x70` w dwóch wariantach (`CMD2=0x30` lekki tik: F|nr w D1, sekundy w D3; `CMD2=0x50` pełny: utwór/min/sek/płyta w D1–D4); w stanie `Playing` co sekundę enqueue przez `TxQueue`; wstrzymać inkrementację w pauzie/seeku
    - _Requirements: 11.1, 11.2, 11.3_
  - [x] 14.2 Zbudować pełny status 0xC0
    - Budowa ramki `0xC0` (long) z numerem płyty, liczbą utworów, minutami i sekundami wg §11.5, wysyłana okresowo jako uzupełnienie `0x90`
    - _Requirements: 11.4_
  - [ ]* 14.3 Napisać property test budowy ramek 0x90
    - **Property 19: Poprawność budowy ramek pozycji 0x90 (oba warianty)**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 19: Ramka 0x90 ma RAD 0x70, pola wg wariantu (0x30: sek w D3, F|nr w D1; 0x50: utwór/min/sek/płyta w D1–D4) i poprawne Parity1/Parity2`
    - **Validates: Requirements 11.1, 11.2**
  - [ ]* 14.4 Napisać property test budowy pełnego statusu 0xC0
    - **Property 20: Poprawność budowy pełnego statusu 0xC0**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 20: Ramka 0xC0 zawiera płytę, liczbę utworów, minuty i sekundy wg §11.5 oraz poprawne sumy kontrolne`
    - **Validates: Requirements 11.4**
  - [ ]* 14.5 Napisać property test wstrzymania licznika czasu
    - **Property 21: Wstrzymanie licznika czasu poza stanem odtwarzania**
    - Komentarz: `Feature: unilink-kompendium-alignment, Property 21: Dla stanu != Playing kolejne aktualizacje czasu nie zwiększają licznika sekund`
    - **Validates: Requirements 11.3**

- [x] 15. Checkpoint — pełna logika protokołu
  - Ensure all tests pass, ask the user if questions arise.

- [x] 16. Dokumentacja zgodności z protokołem i rozbieżności (Wymaganie 12)
  - [ ] 16.1 Dodać komentarze rozbieżności i ryzyka w kodzie
    - Oznaczyć każde odstępstwo `// [DEVIATION §X]` (m.in. `Init→0x80`, fallback skanu) i każdą zmianę dotykającą timingu/struktury ramki `// [HIGH-RISK]` z opisem wpływu i sposobu przywrócenia; zachować istniejące komentarze o strojeniu (okres bitu ~20 µs, bajt ~1 ms, slave-break ~8 ms)
    - _Requirements: 1.4, 1.5, 12.1, 12.2, 12.3_
  - [x] 16.2 Uzupełnić tabelę zgodności w dokumentacji projektowej
    - Zaktualizować/utrzymać zestawienie „Zgodność z protokołem i odstępstwa" z uzasadnieniem technicznym każdego odstępstwa
    - _Requirements: 12.1, 12.2_

- [x] 17. Checkpoint końcowy — kompilacja host i szkic ESP32
  - Ensure all tests pass, ask the user if questions arise.
  - Zweryfikować kompilację testów host-side oraz kompilację szkicu Arduino (`Emulator.ino`); testy na realnym radiu pozostają opcjonalną weryfikacją końcową poza zakresem zadań kodowych.

## Notes

- Zadania oznaczone `*` są opcjonalne (testy) i mogą zostać pominięte przy szybkim MVP, ale każda z 22 właściwości poprawności ma dokładnie jeden test property-based (≥100 iteracji).
- Czysta logika (`UnilinkFrame`, `CdText`, `Magazine`, mapowanie statusu, tryby, nawigacja) kompiluje się natywnie i jest testowana property-based; warstwa sprzętowa (`UnilinkBus` ISR, Slave Break) jest poza testami property-based.
- Każde zadanie odwołuje się do konkretnych podwymagań dla śledzenia pokrycia.
- Checkpointy zapewniają przyrostową walidację; testy na realnym radiu (CDX-M670, MEX-BT3800u) są opcjonalną weryfikacją końcową i nie blokują zadań kodowych.
- Stałe czasowe i komentarze o strojeniu pozostają nietknięte co do wartości (Stan_Wysokiego_Ryzyka oznaczony `// [HIGH-RISK]`).

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1", "1.2"] },
    { "id": 1, "tasks": ["1.3", "1.4", "1.5", "1.6", "1.7", "2.1", "5.1"] },
    { "id": 2, "tasks": ["2.2", "3.1", "4.1", "6.1", "9.1", "9.2", "10.1"] },
    { "id": 3, "tasks": ["3.2", "5.2", "9.4", "9.5", "9.6", "10.3", "10.4"] },
    { "id": 4, "tasks": ["4.2", "5.3", "5.4", "5.5", "6.2"] },
    { "id": 5, "tasks": ["6.3", "6.4", "8.1"] },
    { "id": 6, "tasks": ["8.2"] },
    { "id": 7, "tasks": ["8.3", "9.3"] },
    { "id": 8, "tasks": ["10.2"] },
    { "id": 9, "tasks": ["11.1"] },
    { "id": 10, "tasks": ["11.2"] },
    { "id": 11, "tasks": ["11.3", "12.1", "11.4", "11.5", "11.6"] },
    { "id": 12, "tasks": ["12.2", "13.1"] },
    { "id": 13, "tasks": ["14.1"] },
    { "id": 14, "tasks": ["14.2"] },
    { "id": 15, "tasks": ["13.2", "14.3", "14.4", "14.5", "16.1", "16.2"] }
  ]
}
```
