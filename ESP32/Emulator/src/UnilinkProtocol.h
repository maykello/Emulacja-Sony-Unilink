#ifndef UNILINK_PROTOCOL_H
#define UNILINK_PROTOCOL_H

#include <Arduino.h>

// =============================================================================
// UnilinkProtocol — warstwa aplikacyjna protokolu Sony UniLink
// =============================================================================
// Interpretuje odebrane ramki, prowadzi discovery i przydzial adresu, obsluguje
// kwirki radia CDX-M670 (2-fazowe discovery, okno preliminary) oraz arbitraz
// dostepu do magistrali (`01 15` -> maska -> grant `01 13`). Stan sesji z radiem
// (przydzielony adres, maska arbitrazu, flagi discovery) trzyma w sobie. Zalezy
// od UnilinkBus (wysylanie/break) i CdChanger (stan zmieniarki).
// =============================================================================

namespace UnilinkProtocol {

// Inicjalizacja licznikow sesji. Wywolac w setup().
void begin();

// Przetworz pojedyncza, kompletna ramke odebrana z magistrali.
void handlePacket(const uint8_t* buf, int len);

// Zakolejkuj gotowa ramke (z parzystosciami) do nadania po grancie request-poll
// `0x01 0x13` (Kompendium §7.2 / §12.4). `priority` to jeden ze stalych
// Tx::PRIO_* (nizszy numer = wyzszy priorytet); `bytes`/`len` to kompletna
// ramka. O glos prosimy w arbitrazu `0x01 0x15` (maska urzadzenia), a nie
// Slave Breakiem. Time-poll PONG (`0x01 0x12`) NIE idzie przez kolejke —
// odpowiadamy na niego natychmiast. Uzywane przez modul CD-TEXT, magazynka,
// ikon i ramki czasu.
void enqueue(uint8_t priority, const uint8_t* bytes, int len);

// Zbuduj i zakolejkuj ramke identyfikatora plyty (long, 9 bajtow danych) z
// Magazine::buildDiscId(disc, ...) i priorytetem Tx::PRIO_DISC_ID. Adresowana
// do mastera (RAD=0x10) — to dane dla logiki Custom File radia, nie tresc
// ekranu. `discChangeVariant` wybiera CMD1: 0xD5 przy zmianie plyty w trakcie
// pracy, 0xC5 przy skanie magazynka (R7.3). Disc ID jest staly per plyta (R7.5).
void enqueueDiscId(uint8_t disc, bool discChangeVariant = true);

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
// Struktura ramki 0xC0 (long, 16B) — odtworzona ze sniffu CDX-M670:
//   70 <addr> C0 <status> | P1 | 00 00 00 00 00 <TRK> <MIN> <SEK> <DISC> | P2 | 00
//     CMD2   : bajt statusu mechanizmu (0x00 gra / 0x20 zmiana / 0x40 ladowanie
//              / 0x80 idle / 0xC0 wysuwanie) — ten sam co w PONG na `01 12`
//     D1..D5 : 0x00
//     D6     : numer utworu (BCD z F-paddingiem)
//     D7     : minuty (BCD z F-paddingiem), 0xFF gdy czas nieznany
//     D8     : sekundy (BCD), 0xFF gdy czas nieznany
//     D9     : numer plyty w gornym nibblu, flaga zmiany w dolnym
// Zmiana ukladu wymaga aktualizacji Parity1/Parity2 wg wzorow w UnilinkFrame.h.

// Zbuduj i zakolejkuj ramke 0xC0 z priorytetem Tx::PRIO_STATUS.
void enqueueFullStatusFrame();

// Harmonogram 1Hz dla ramki 0x90 — wywoływać w loop() przy BUS=1.
// W stanie Playing co sekundę zakolekcja 0x90 (light tick) przez TxQueue.
// W stracie (Seeking/ChangedCd/LoadingTrack) nie aktualizuje czasu.
void servicePositionFrame1Hz(unsigned long now);

// Harmonogram Slave Break — wywolywac w kazdej iteracji loop() (gdy BUS=1).
void serviceSlaveBreak(bool busPowered);

// Wypycha CD-TEXT (nazwa utworu 0xD2 + nazwa plyty 0xDA) po kazdej zmianie
// utworu/plyty, bez czekania na zadanie radia — tak robi prawdziwa zmieniarka.
// Wywolywac w loop() przy BUS=1.
void serviceCdText(unsigned long now);

// Zapomnij, ktore nazwy juz wyslano (nowa sesja: BUS off / appoint / reset).
void resetCdTextCache();

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

// Zapisuje stan adresacji do NVS, jesli nastapila zmiana. Wywolywac w loop().
void servicePersist();

// Wysłać ramki pozycji i pełnego statusu natychmiast — używana przy 0x08
// (zakończenie przewijania) do natychmiastowego odświeżenia ekranu radia.
void sendDisplayStatus();

} // namespace UnilinkProtocol

#endif // UNILINK_PROTOCOL_H