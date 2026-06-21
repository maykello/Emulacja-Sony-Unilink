#ifndef UNILINK_PROTOCOL_H
#define UNILINK_PROTOCOL_H

#include <Arduino.h>

// =============================================================================
// UnilinkProtocol — warstwa aplikacyjna protokolu Sony UniLink
// =============================================================================
// Interpretuje odebrane ramki, prowadzi discovery i przydzial adresu, obsluguje
// kwirki radia CDX-M670 (2-fazowe discovery, okno preliminary) oraz harmonogram
// Slave Break. Stan sesji z radiem (przydzielony adres, flagi discovery) trzyma
// w sobie. Zalezy od UnilinkBus (wysylanie/break) i CdChanger (stan zmieniarki).
// =============================================================================

namespace UnilinkProtocol {

// Inicjalizacja licznikow sesji. Wywolac w setup().
void begin();

// Przetworz pojedyncza, kompletna ramke odebrana z magistrali.
void handlePacket(const uint8_t* buf, int len);

// Zakolejkuj gotowa ramke (z parzystosciami) do nadania po grancie request-poll
// `0x01 0x13` (Kompendium §7.2 / §12.4). `priority` to jeden ze stalych
// Tx::PRIO_* (nizszy numer = wyzszy priorytet); `bytes`/`len` to kompletna
// ramka. Po zakolejkowaniu kolejny serviceSlaveBreak wystawi Slave Break, by
// radio przyznalo nam glos. Time-poll PONG (`0x01 0x12`) NIE idzie przez
// kolejke — odpowiadamy na niego natychmiast. Uzywane przez modul CD-TEXT,
// magazynka, ikon i ramki czasu (zadania 9.x/10.x/11.x/13.x/14.x).
void enqueue(uint8_t priority, const uint8_t* bytes, int len);

// Zbuduj i zakolejkuj ramke identyfikatora plyty `0xD5` (long, 9 bajtow danych)
// z Magazine::buildDiscId(disc, ...) i priorytetem Tx::PRIO_DISC_ID. Wywolywane
// przy skanie magazynka / zmianie plyty (R7.3); disc ID jest staly per plyta
// (R7.5). Wystawione publicznie, by kod wykrywajacy zmiane plyty (CdChanger /
// bezposredni wybor 0xB0 — inne zadania) mogl je wywolac bez duplikowania
// logiki budowania ramki.
void enqueueDiscId(uint8_t disc);

// Pomocnik do zakolejkowania ramki ikon 0x94. Wywolywane przez CdChanger
// po kazdej zmianie trybu (toggleRepeat/Shuffle/Intro lub auto-advance).
// Buduje gotowa ramke 0x94 (middle, 11B, RAD=0x70, TAD=myAddr, CMD1=0x94)
// i zakolekcja ja w TxQueue z priorytetem Tx::PRIO_MAGAZINE.
// Stan trybow (repeatMode, shuffle, intro) jest kodowany w D1..D4 przez
// UnilinkFrame::encodeIconData. Ta funkcja jest uzywana przez CdChanger,
// ktory nie ma dostepu do globalnego myAddr i enqueue().
void enqueueModeIconsHelper(uint8_t repeatMode, bool shuffle, bool intro);

// ============================================================
// POMOCNIKI DO RAMKI POZYCJI 0x90 (Wymaganie 11 - Zadanie 14.1)
// ============================================================
// Rama 0x90 wysyłana co sekundę w stanie Playing. Dwa warianty:
//   - CMD2=0x30 (lekki tik): D1=F|nr, D3=sekundy (jak CDX-805 sniff)
//   - CMD2=0x50 (pełny): D1=utwór, D2=minuty, D3=sekundy, D4=F|nr
// Adresowanie: RAD=0x70 (wyswietlacz), TAD=myAddr, priorytet Tx::PRIO_TIME (4).
// Wstrzymuje inkrementację w non-playing states (Seeking/ChangedCd/LoadingTrack).

// Zbuduj i zakolejkuj ramkę 0x90 z wariantem CMD2=0x30 (lekki tik).
// Sekundy w D3, numer płyty F|nr w D1 (jak sniff CDX-805).
// Używana przy każdej sekundzie w stanie Playing.
void enqueuePositionFrameLightTick();

// Zbuduj i zakolejkuj ramkę 0x90 z wariantem CMD2=0x50 (pełny).
// Utwór/minuty/sekundy/płyta w D1-D4.
// Używana przy zmianie stanu/dysku/utworu dla natychmiastowej aktualizacji.
void enqueuePositionFrameFull();

// ============================================================
// POMOCNIKI DO RAMKI PEŁNEGO STATUSU 0xC0 (Wymaganie 11 - Zadanie 14.2)
// ============================================================
// Rama 0xC0 (long, 16B) wysyłana okresowo w stanie Playing, uzupełniając lekki
// tik 0x90. Zawiera numer płyty, liczbę utworów, minuty i sekundy wg §11.5.
//
// Struktura ramki 0xC0 (long):
//   Byte 0-3: RAD TAD CMD1 CMD2
//   Byte 4:   Parity1 = (RAD+TAD+CMD1+CMD2) mod 256
//   Byte 5-13: D1..D9 = dane (kompatybilne z CDX-805 sniff)
//     D1 (D2_1): rezerwa (0x00)
//     D2 (D2_2): rezerwa (0x00)
//     D3 (D2_3): rezerwa (0x00)
//     D4 (D2_4): rezerwa (0x00)
//     D5 (D2_5): liczba utworów (BCD)
//     D6 (D2_6): minuty (BCD)
//     D7 (D2_7): sekundy (BCD)
//     D8 (D2_8): numer płyty (F|nr)
//     D9 (D2_9): rezerwa (0x00)
//   Byte 14:  Parity2 = (Parity1 + D1..D9) mod 256
//   Byte 15:  END = 0x00
// [HIGH-RISK] Struktura ramki 0xC0 (długość 16B, parzystości, D1..D9) jest
// zgodna z Kompendium §11.5. Zmiana struktury ramki wymaga aktualizacji
// Parity1/Parity2 wedlug wzorów w UnilinkFrame.h.

// Zbuduj i zakolejkuj ramkę 0xC0 (long, pełny status) z numerem płyty,
// liczbą utworów, minutami i sekundami wg Kompendium §11.5.
// RAMKA: RAD=0x70, TAD=myAddr, CMD1=0xC0, CMD2=0x00, Parity1, D1..D9, Parity2, 0
// D1-D4: rezerwa (0x00), D5 (D2_1): rezerwa (0x00), D6 (D2_2): liczba utworów,
// D7 (D2_3): minuty (BCD), D8 (D2_4): sekundy (BCD), D9 (D2_5): numer płyty (F|nr).
// Priorytet: Tx::PRIO_STATUS (najwyższy, pierwszy w kolejce po 0x90).
void enqueueFullStatusFrame();

// Harmonogram 1Hz dla ramki 0x90 — wywoływać w loop() przy BUS=1.
// W stanie Playing co sekundę zakolekcja 0x90 (light tick) przez TxQueue.
// W stracie (Seeking/ChangedCd/LoadingTrack) nie aktualizuje czasu.
void servicePositionFrame1Hz(unsigned long now);

// Harmonogram Slave Break — wywolywac w kazdej iteracji loop() (gdy BUS=1).
void serviceSlaveBreak(bool busPowered);

// Harmonogram ramki 0xC0 (pełny status) — wywoływać w loop() przy BUS=1.
// W stanie Playing co OKRES_WYSYLANIA zakolekcja 0xC0 (long, pełny status).
// Wstrzymuje wysyłanie w non-playing states.
void serviceFullStatusFrame(unsigned long now);

// Sprawdza czy radio zniklo (brak PINGa > RADIO_TIMEOUT_MS). Jesli tak —
// resetuje stan sesji i zwraca true (raz). Wywolywac w loop().
bool serviceTimeout(unsigned long now);

// Reset sesji przy zaniku zasilania magistrali (BUS=0).
void onBusOff();

// Czy radio przydzielilo nam adres (sesja aktywna)?
bool isAllocated();

// Lekka diagnostyka: co ~2s wypisuje podsumowanie ruchu (ile razy radio pytalo
// nas o status/ekran, ile breakow, czy oddaje ekran 3B). Wywolywac w loop().
void serviceStats(unsigned long now);

// Wysłać ramki pozycji i pełnego statusu natychmiast — używana przy 0x08
// (zakończenie przewijania) do natychmiastowego odświeżenia ekranu radia.
void sendDisplayStatus();

} // namespace UnilinkProtocol

#endif // UNILINK_PROTOCOL_H