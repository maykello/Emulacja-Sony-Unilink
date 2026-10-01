#ifndef UNILINK_PROTOCOL_H
#define UNILINK_PROTOCOL_H

#include <Arduino.h>

// =============================================================================
// UnilinkProtocol — zmieniarka CD na magistrali Sony Unilink (zachowanie OE)
// =============================================================================
// Dziala we wlasnym zadaniu FreeRTOS (rdzen 1, wysoki priorytet), niezaleznie od
// petli glownej, audio i USB. Odtwarza zachowanie prawdziwej zmieniarki Sony
// zarejestrowane na CDX-M670 (sniffy w "Python logger/unilink_log_zmieniarka_*"):
//
//  * discovery: CDX-M670 najpierw odpytuje swoje wewnetrzne urzadzenia przy
//    BUS_ON=0, a zewnetrzne dopiero po BUS_ON=1 — przy BUS_ON=0 milczymy,
//  * ANYONE? (18 10 01 02) -> 10 30 8C ..., appoint 3X 10 02 YY -> 10 3X 8C ...,
//  * nieprzydzielona zmieniarka odpowiada na 18 10 01 11 ramka 10 18 04 00 —
//    radio robi wtedy reset magistrali i ja dopisuje (tak dolacza zmieniarka,
//    ktora wystartowala po discovery radia),
//  * adres trzymamy do resetu magistrali (18 10 01 00); krotkie zaniki BUS_ON
//    NIE kasuja sesji — radio po nich dalej pinguje znany adres,
//  * w trakcie odtwarzania co sekunde: Slave Break -> 18 10 01 15 -> 82 <maska>
//    -> grant 3X 10 01 13 -> ramka czasu 70 3X 90 ...,
//  * kazda kolejna ramka bloku (status C0, nazwy D2/DA/D7, ID plyty) dostaje
//    wlasny Break, jak w oryginale (~20-80 ms na ramke).
// =============================================================================

namespace UnilinkProtocol {

// Wczytuje adres z NVS i uruchamia zadanie magistrali. Wolac w setup() po
// CdChanger::begin().
void begin();

// Czy radio przydzielilo nam adres?
bool isAllocated();

// Czy radio ma nas wybranych jako zrodlo (PLAY / 18 10 F0 <nasz adres>)?
// Kasowane przez wybor innego zrodla (F0) i wylaczenie audio (87, bit 0 = 0).
bool isSelected();

// Stan BUS_ON po filtracji (true = radio wlaczone).
bool busActive();

// Kolejkowanie ramek nadawanych na kolejne granty. Wolac tylko z zadania
// magistrali (CdChanger dziala w jego kontekscie).
void enqueue(uint8_t priority, const uint8_t* bytes, int len);
void enqueueModeIconsHelper(uint8_t repeatMode, bool shuffle, bool intro);

// Czy petla glowna moze teraz zapisac flash (NVS)? Zapis wstrzymuje zadanie
// magistrali, wiec robimy go tuz po naszej odpowiedzi na ping (nastepny ping
// za ~2.4 s) albo gdy radio jest wylaczone.
bool flashWriteWindow();

// Zapis przydzielonego adresu do NVS (petla glowna).
void servicePersist(bool flashWriteAllowed);
void persistNow();

} // namespace UnilinkProtocol

#endif // UNILINK_PROTOCOL_H
