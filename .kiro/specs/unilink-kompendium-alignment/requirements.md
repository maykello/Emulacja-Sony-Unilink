# Requirements Document

## Introduction

Projekt to emulator zmieniarki CD Sony UniLink działający na ESP32 (Arduino C++).
Zmieniarka UniLink ze swojej natury współpracuje z dowolnym radiem Sony obsługującym
tę magistralę, więc emulator również ma być **uniwersalny**. Użytkownik dostarczył
dokument `UNILINK_PROTOKOL_KOMPENDIUM.md`, który opisuje protokół magistrali w sposób
ogólny (na bazie sniffu zmieniarki CDX-805, dokumentacji Mictronics i działających
emulatorów open-source).

Celem tej specyfikacji jest **uzgodnienie (alignment) emulatora z protokołem UniLink
opisanym w Kompendium**: doprowadzenie implementacji do zgodności z faktycznym
działaniem magistrali, tak aby emulator współpracował z dowolnym radiem Sony.

Założenia nadrzędne (obowiązują w całym dokumencie):

1. **Źródłem prawdy jest protokół magistrali UniLink opisany w Kompendium.** Docelowe
   zachowanie emulatora wynika ze zgodności z protokołem, a nie z empirycznych kwirków
   pojedynczego modelu radia.
2. **Celem nadrzędnym jest poprawna, uniwersalna implementacja protokołu UniLink**
   działająca z każdym radiem Sony. Kompatybilność z konkretnymi radiami testowymi
   (CDX-M670, MEX-BT3800u) wynika z poprawności protokołu, a nie jest nadrzędnym
   ograniczeniem blokującym poprawne zachowanie.
3. **Udokumentowane warianty modelowe są częścią protokołu, nie trybem pod jedno
   radio.** Tam, gdzie Kompendium opisuje dopuszczalne warianty (np. ramka czasu
   `0x90` z `CMD2=0x30` albo `CMD2=0x50`, albo pełny status `0xC0`), emulator obsługuje
   i wysyła je zgodnie z opisem protokołu, dobierając wariant według kontekstu.
4. **Bezpiecznik regresji jest zasadą inżynierską, nie nadrzędnym wetem.** Zmiany nie
   powinny bez powodu wprowadzać regresji we współpracy z realnym radiem testowym, ale
   poprawność protokołu ma pierwszeństwo. Wiedza o warstwie fizycznej (strojenie czasów
   Slave Break, ciche okna idle) jest realna i wynika z protokołu (okres bitu ~20 µs,
   bajt ~1 ms, slave-break w oknie idle ~8 ms — patrz Kompendium §1, §2), więc komentarze
   ostrzegawcze o timingu warto zachować jako wiedzę inżynierską.

Zakres dotyczy plików w `ESP32/Emulator/` (Config.h, UnilinkBus.cpp/.h,
UnilinkProtocol.cpp/.h, CdChanger.cpp/.h oraz powiązanych modułów).

## Glossary

- **Emulator**: cały system oprogramowania zmieniarki CD na ESP32 będący
  przedmiotem tej specyfikacji.
- **Radio**: urządzenie master magistrali UniLink (głowica), które odpytuje i
  steruje Emulatorem. Emulator ma współpracować z dowolnym radiem Sony obsługującym
  UniLink (radia testowe to CDX-M670 i MEX-BT3800u).
- **Kompendium**: dokument `UNILINK_PROTOKOL_KOMPENDIUM.md` opisujący protokół
  magistrali UniLink; w tej specyfikacji traktowany jako źródło prawdy.
- **Protokol_UniLink**: zbiór reguł magistrali (warstwa fizyczna, struktura ramek,
  adresowanie, cykl pracy, komendy) opisany w Kompendium.
- **Wariant_Protokolu**: udokumentowany w Kompendium alternatywny sposób realizacji
  tej samej funkcji (np. ramka `0x90` z `CMD2=0x30` vs `CMD2=0x50`), dopuszczalny przez
  protokół i dobierany według kontekstu, a nie przypisany do jednego modelu radia.
- **Walidator_Ramek**: część Emulatora odpowiedzialna za sprawdzanie poprawności
  odebranej ramki (sumy kontrolne, bajt końcowy).
- **Parser_Ramek**: część Emulatora w warstwie odbioru (UnilinkBus) składająca
  surowe bajty w kompletne ramki.
- **Menedzer_Adresow**: część Emulatora odpowiedzialna za dynamiczny przydział i
  adopcję adresu UniLink.
- **Modul_CD_TEXT**: część Emulatora odpowiedzialna za przechowywanie i
  wysyłanie nazw płyt i utworów.
- **Modul_Magazynka**: część Emulatora raportująca obecność płyt i identyfikatory
  płyt.
- **Ramka**: pojedynczy „word" UniLink (short 6B / middle 11B / long 16B) wraz z
  bajtem końcowym `0`.
- **Parity1**: suma kontrolna nagłówka = `(RAD + TAD + CMD1 + CMD2) mod 256`.
- **Parity2**: druga suma kontrolna = `(Parity1 + suma bajtów danych) mod 256`.
- **RAD**: adres odbiorcy ramki (Receiver Address).
- **TAD**: adres nadawcy ramki (Transmitter Address).
- **CMD1**: główny bajt komendy; wyznacza długość ramki.
- **Stan_Wysokiego_Ryzyka**: zmiana dotykająca wartości czasowych, struktury
  bitowej ramki lub mechanizmu Slave Break.

## Requirements

### Wymaganie 1: Zgodność z protokołem UniLink i uniwersalność

**User Story:** Jako właściciel emulatora, chcę by emulator był zgodny z protokołem
magistrali UniLink opisanym w Kompendium, aby współpracował z dowolnym radiem Sony,
a nie tylko z jednym modelem.

#### Kryteria akceptacji

1. THE Emulator SHALL realizować zachowanie magistrali zgodnie z Protokol_UniLink
   opisanym w Kompendium (warstwa fizyczna, struktura ramek, adresowanie, cykl pracy,
   komendy).
2. WHERE Kompendium opisuje Wariant_Protokolu dla danej funkcji, THE Emulator SHALL
   obsługiwać lub wysyłać ten wariant zgodnie z opisem protokołu, dobierając go według
   kontekstu komunikacji, a nie według pojedynczego modelu radia.
3. WHEN Emulator nawiązuje sesję z Radiem zgodnym z UniLink, THE Emulator SHALL
   utrzymać poprawną sesję (przydział adresu, odpowiedzi na poll i status) dzięki
   zgodności z Protokol_UniLink.
4. IF zmiana zgodna z Protokol_UniLink wprowadziłaby regresję we współpracy z realnym
   radiem testowym, THEN THE Emulator SHALL udokumentować przyczynę regresji i jej
   rozwiązanie zgodne z protokołem, zamiast cofać się do zachowania niezgodnego z
   protokołem bez uzasadnienia.
5. WHERE zmiana jest Stanem_Wysokiego_Ryzyka, THE Emulator SHALL jawnie oznaczyć ją w
   kodzie wraz z opisem wpływu na timing magistrali i sposobem przywrócenia wartości
   strojenia.

### Wymaganie 2: Walidacja obu sum kontrolnych i bajtu końcowego

**User Story:** Jako integrator magistrali, chcę by emulator odrzucał ramki z
błędnymi sumami kontrolnymi zgodnie z kompendium (§4, §12.2), aby przekłamane
ramki nie wywoływały błędnych reakcji.

#### Kryteria akceptacji

1. WHEN odebrana Ramka ma długość middle lub long, THE Walidator_Ramek SHALL
   sprawdzić zarówno Parity1, jak i Parity2.
2. IF Parity1 odebranej Ramki nie jest równe `(RAD + TAD + CMD1 + CMD2) mod 256`,
   THEN THE Walidator_Ramek SHALL odrzucić Ramkę.
3. IF Parity2 odebranej Ramki (middle lub long) nie jest równe `(Parity1 + suma
   bajtów danych) mod 256`, THEN THE Walidator_Ramek SHALL odrzucić Ramkę.
4. WHEN odebrana Ramka ma długość short, THE Walidator_Ramek SHALL sprawdzić
   wyłącznie Parity1 (Parity2 nie występuje w short word).
5. THE Walidator_Ramek SHALL traktować odrzucenie Ramki jako zdarzenie
   diagnostyczne rejestrowane w module Diagnostyki.

### Wymaganie 3: Wyznaczanie długości ramki na podstawie CMD1

**User Story:** Jako integrator magistrali, chcę by warstwa odbioru rozpoznawała
długość ramki po wartości CMD1 zgodnie z kompendium (§3), aby ramki były
parsowane jako kompletne jednostki, a nie tylko sklejane po ciszy.

#### Kryteria akceptacji

1. WHEN Parser_Ramek odebrał bajt CMD1 o wartości mniejszej niż `0x80`, THE
   Parser_Ramek SHALL traktować oczekiwaną długość Ramki jako 6 bajtów.
2. WHEN Parser_Ramek odebrał bajt CMD1 o wartości z zakresu od `0x80` do `0xBF`,
   THE Parser_Ramek SHALL traktować oczekiwaną długość Ramki jako 11 bajtów.
3. WHEN Parser_Ramek odebrał bajt CMD1 o wartości równej lub większej niż `0xC0`,
   THE Parser_Ramek SHALL traktować oczekiwaną długość Ramki jako 16 bajtów.
4. WHEN liczba odebranych bajtów osiągnie długość wyznaczoną przez CMD1, THE
   Parser_Ramek SHALL udostępnić kompletną Ramkę do przetworzenia.
5. WHERE warstwa odbioru wykorzystuje wykrywanie ciszy magistrali, THE
   Parser_Ramek SHALL używać wyznaczania długości po CMD1 jako podstawowego
   kryterium granicy Ramki, a ciszy wyłącznie jako zabezpieczenia awaryjnego.

### Wymaganie 4: Dynamiczne adresowanie zgodne z modelem grup

**User Story:** Jako integrator magistrali, chcę by emulator startował jako
urządzenie grupy CD bez ID i przyjmował adres przydzielony przez radio (§5, §6),
aby poprawnie współpracować z dowolnym zestawem urządzeń na magistrali.

#### Kryteria akceptacji

1. WHEN Emulator startuje przed przydziałem adresu, THE Menedzer_Adresow SHALL
   używać adresu źródłowego `0x30` (grupa CD, brak ID) zgodnie z Protokol_UniLink.
2. WHEN Radio wysyła broadcast „Anyone?" (`CMD1=0x01, CMD2=0x02`) i Emulator nie
   ma przydzielonego ID, THE Menedzer_Adresow SHALL odpowiedzieć stringiem
   informacyjnym urządzenia (`CMD1=0x8C`).
3. WHEN Radio wysyła Appoint (`CMD1=0x02`) z RAD należącym do grupy CD (`0x3X`),
   THE Menedzer_Adresow SHALL zapamiętać przydzielony adres i używać go jako TAD
   we wszystkich kolejnych odpowiedziach.
4. WHEN Radio wysyła reset magistrali (`CMD1=0x01, CMD2=0x00`), THE
   Menedzer_Adresow SHALL przywrócić adres źródłowy `0x30`.
5. WHILE Emulator nie otrzymał jeszcze przydziału ID od Radia, THE Menedzer_Adresow
   SHALL używać adresu `0x30` jako TAD i nie przyjmować z góry żadnego stałego ID.

### Wymaganie 5: Uzgodnienie kodów statusu odtwarzania z protokołem

**User Story:** Jako programista protokołu, chcę by kody statusu emulatora były
zgodne z semantyką kompendium (§7.1), aby radio interpretowało stany odtwarzania
poprawnie.

#### Kryteria akceptacji

1. THE Emulator SHALL stosować kody statusu zgodne z semantyką Kompendium §7.1
   (`0x00` Playing, `0x20` Changed CD, `0x21` Seeking, `0x40` Changing CD, `0x80`
   Idle, `0xC0` Ejecting) jako mapowanie docelowe.
2. WHEN Emulator rozpoczyna przewijanie, THE Emulator SHALL zwracać przy time-pollach
   status `0x21` (Seeking) zgodnie z Protokol_UniLink.
3. WHEN Emulator rozpoczyna zmianę płyty, THE Emulator SHALL zwracać status `0x40`
   (Changing CD), a po jej zakończeniu `0x20` (Changed CD), a następnie `0x00`
   (Playing), zgodnie z maszyną stanów z Kompendium §7.1.
4. WHERE bieżący kod statusu Emulatora odbiega od semantyki Kompendium (np. użycie
   `0xC0` jako stanu inicjalizacji albo `0x20` jako przewijania), THE Emulator SHALL
   uzgodnić ten kod z Protokol_UniLink, a jeżeli odstępstwo zostaje zachowane, SHALL
   udokumentować techniczne uzasadnienie tego odstępstwa.

### Wymaganie 6: Odpowiedzi CD-TEXT (nazwy płyt i utworów)

**User Story:** Jako użytkownik radia z obsługą CD-TEXT, chcę by emulator
odpowiadał na żądania nazw płyt i utworów (§10), aby na wyświetlaczu pojawiały
się tytuły zamiast braku tekstu.

#### Kryteria akceptacji

1. WHEN Radio wysyła żądanie nazwy utworu (`CMD1=0x84, CMD2=0xD9`) z numerem pola
   w D1, THE Modul_CD_TEXT SHALL odesłać żądane pole nazwy utworu.
2. WHEN Radio wysyła żądanie nazwy płyty (`CMD1=0x84, CMD2=0xDD`) z numerem pola
   w D1, THE Modul_CD_TEXT SHALL odesłać żądane pole nazwy płyty.
3. WHEN numer żądanego pola należy do zakresu 0–1, THE Modul_CD_TEXT SHALL użyć
   komendy `0xC9` dla nazwy utworu albo `0xCD` dla nazwy płyty.
4. WHEN numer żądanego pola należy do zakresu 2–5, THE Modul_CD_TEXT SHALL użyć
   komendy `0xD9` dla nazwy utworu albo `0xDD` dla nazwy płyty.
5. WHEN numer żądanego pola przekracza 5 lub tekst się zakończył, THE
   Modul_CD_TEXT SHALL zakończyć przesyłanie kolejnych pól.
6. THE Modul_CD_TEXT SHALL ograniczać wysyłane znaki do drukowalnego ASCII
   (zakres `0x20`–`0x7E`).
7. WHERE kontekst komunikacji wskazuje wariant CD-TEXT w trybie CD (`0xD2`, pierwszy
   znak w CMD2, 6 znaków na ramkę), THE Modul_CD_TEXT SHALL obsłużyć ten
   Wariant_Protokolu zgodnie z Kompendium §10.3.
8. FOR ALL nazw płyt i utworów ograniczonych do drukowalnego ASCII, podzielenie
   nazwy na pola, wysłanie ich i ponowne złożenie z pól SHALL odtworzyć tę samą
   nazwę (właściwość round-trip).

### Wymaganie 7: Mapa magazynka, obecność płyt i identyfikatory płyt

**User Story:** Jako użytkownik radia, chcę by emulator raportował obecność płyt i
ich identyfikatory (§8.2, C.3, C.4), aby radio poprawnie pokazywało dostępne
płyty i dopasowywało CD-TEXT.

#### Kryteria akceptacji

1. WHEN Radio żąda informacji o magazynku (`CMD1=0x84, CMD2=0x95`), THE
   Modul_Magazynka SHALL odesłać mapę obecności płyt (`CMD1=0x95`).
2. WHEN Radio żąda informacji o płycie (`CMD1=0x84, CMD2=0x97`), THE
   Modul_Magazynka SHALL odesłać liczbę utworów oraz czas całkowity płyty
   (`CMD1=0x97`).
3. WHEN następuje skan magazynka lub zmiana płyty, THE Modul_Magazynka SHALL
   wysłać identyfikator płyty (`CMD1=0xC5` lub `0xD5`).
4. THE Modul_Magazynka SHALL kodować numer płyty jako górny nibble `F` i numer w
   dolnym nibblu (`F1`=1 … `F4`=4 …).
5. THE Modul_Magazynka SHALL utrzymywać identyfikator płyty stały dla danej płyty
   pomiędzy żądaniami.

### Wymaganie 8: Tryby Repeat / Shuffle / Intro i ramka ikon

**User Story:** Jako użytkownik radia, chcę by emulator obsługiwał przełączanie
trybów Repeat/Shuffle/Intro (§8.1, §9.4), aby radio pokazywało aktualne ikony
trybów.

#### Kryteria akceptacji

1. WHEN Radio wysyła komendę Toggle Repeat (`CMD1=0x34`), THE Emulator SHALL
   przełączyć tryb Repeat na kolejną wartość.
2. WHEN Radio wysyła komendę Toggle Shuffle (`CMD1=0x35`), THE Emulator SHALL
   przełączyć tryb Shuffle.
3. WHEN Radio wysyła komendę Toggle Intro (`CMD1=0x36`), THE Emulator SHALL
   przełączyć tryb Intro.
4. WHEN tryb Repeat, Shuffle lub Intro zmienia wartość, THE Emulator SHALL wysłać
   ramkę ikon trybów (`CMD1=0x94`) odzwierciedlającą bieżący stan trybów.
5. THE Emulator SHALL odzwierciedlać wybrany tryb Repeat w rzeczywistym
   zachowaniu odtwarzania (kolejność utworów / powtarzanie).

### Wymaganie 9: Bezpośredni wybór płyty (0xB0)

**User Story:** Jako użytkownik radia, chcę móc wybrać płytę bezpośrednio (§9.4),
aby przeskoczyć do wskazanej płyty i utworu jednym poleceniem.

#### Kryteria akceptacji

1. WHEN Radio wysyła komendę bezpośredniego wyboru płyty (`CMD1=0xB0`) z numerem
   płyty w CMD2 i numerem utworu w D1, THE Emulator SHALL ustawić bieżącą płytę
   na wartość z CMD2.
2. WHEN komenda `0xB0` wskazuje numer utworu w D1, THE Emulator SHALL ustawić
   bieżący utwór na wartość z D1.
3. IF wskazany numer płyty lub utworu jest poza dostępnym zakresem, THEN THE
   Emulator SHALL zignorować komendę i zachować bieżącą płytę oraz utwór.
4. WHEN bezpośredni wybór płyty zostanie wykonany, THE Emulator SHALL wysłać
   zaktualizowany status odtwarzania.

### Wymaganie 10: Przewijanie i zakończenie broadcastem 0x08

**User Story:** Jako programista protokołu, chcę by emulator obsługiwał model
„press/hold/release" przewijania zgodnie z protokołem (§9.1), aby przewijanie
kończyło się poprawnie po zwolnieniu klawisza na dowolnym radiu.

#### Kryteria akceptacji

1. WHEN Radio wysyła komendę przewijania w przód (`CMD1=0x24`) lub w tył
   (`CMD1=0x25`), THE Emulator SHALL rozpocząć przewijanie w odpowiednim kierunku
   i ustawić status `0x21` (Seeking).
2. WHEN Radio wysyła broadcast Key off (`CMD1=0x08, CMD2=0x00`), THE Emulator SHALL
   zakończyć przewijanie, wrócić do stanu odtwarzania (`0x00`) i wysłać
   zaktualizowaną pozycję zgodnie z Protokol_UniLink.
3. IF Radio nie wysyła broadcastu `0x08` kończącego przewijanie, THEN THE Emulator
   SHALL zastosować skanowanie zatrzaskowe jako zachowanie awaryjne, zachowując
   model `0x08` jako podstawowy mechanizm zgodny z protokołem.
4. THE Emulator SHALL udokumentować obsługę zakończenia przewijania (model `0x08`
   jako podstawowy oraz skanowanie zatrzaskowe jako fallback) zgodnie z Kompendium §9.

### Wymaganie 11: Ramka pozycji/czasu 0x90 z licznikiem sekund

**User Story:** Jako użytkownik radia, chcę by emulator wysyłał ramkę pozycji
`0x90` z licznikiem sekund 1 Hz (§11), aby czas utworu płynął na wyświetlaczu
zgodnie z protokołem, w którym to zmieniarka prowadzi zegar.

#### Kryteria akceptacji

1. WHILE Emulator jest w stanie odtwarzania, THE Emulator SHALL wysyłać ramkę
   pozycji (`CMD1=0x90`) na adres wyświetlacza `0x70` z liczbą sekund aktualizowaną
   co sekundę, zgodnie z Protokol_UniLink (§11.1).
2. WHEN Emulator buduje ramkę `0x90`, THE Emulator SHALL umieścić numer płyty,
   znacznik minut oraz upływające sekundy w polach zgodnych z wybranym
   Wariant_Protokolu (`CMD2=0x30` — lekki tik sekund jak w §11.2, albo `CMD2=0x50`
   — pełny utwór/min/sek jak w §11.3).
3. WHILE odtwarzanie jest wstrzymane lub trwa przewijanie, THE Emulator SHALL
   wstrzymać inkrementację licznika sekund.
4. WHERE potrzebny jest pełny status (numer płyty, liczba utworów, minuty, sekundy),
   THE Emulator SHALL wysyłać dodatkowo ramkę `0xC0` zgodnie z Kompendium §11.5,
   uzupełniając lekki tik czasu `0x90`.

### Wymaganie 12: Dokumentacja zgodności z protokołem i weryfikacja regresji

**User Story:** Jako opiekun projektu, chcę by zgodność emulatora z protokołem oraz
wszelkie odstępstwa były udokumentowane, a zmiany weryfikowalne, aby decyzje były
świadome i odwracalne.

#### Kryteria akceptacji

1. THE Emulator SHALL zawierać w kodzie lub dokumentacji projektowej zestawienie
   zgodności z Kompendium oraz wszelkich odstępstw od Protokol_UniLink wraz z
   uzasadnieniem technicznym każdego odstępstwa.
2. WHERE wprowadzono Stan_Wysokiego_Ryzyka, THE Emulator SHALL opisać ryzyko dla
   timingu magistrali oraz sposób przywrócenia dotychczasowych wartości strojenia.
3. THE Emulator SHALL zachować istniejące komentarze ostrzegawcze dotyczące
   strojenia czasów i ramek jako wiedzę o warstwie fizycznej magistrali (okres bitu
   ~20 µs, bajt ~1 ms, slave-break w oknie idle ~8 ms — Kompendium §1, §2).
