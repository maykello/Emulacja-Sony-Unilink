#ifndef UNILINK_PARSER_MODEL_H
#define UNILINK_PARSER_MODEL_H

#include <stdint.h>
#include <stddef.h>

// =============================================================================
// UnilinkParserModel — czysty model parsera ramek UniLink (bez ISR/sprzetu)
// =============================================================================
// Modul CELOWO nie zalezy od Arduino.h ani globalnego stanu sprzetu — tylko od
// <stdint.h>/<stddef.h>. Odwzorowuje *kryterium podstawowe* cieciu strumienia z
// UnilinkBus::readFrame: granica ramki jest wyznaczana przez dlugosc z CMD1
// (rxBuffer[2]), a NIE przez cisze magistrali. Dzieki wydzieleniu tej logiki do
// czystej funkcji mozna ja testowac property-based natywnie na hoscie
// (Wymaganie 3.4, 3.5, Property 4), niezaleznie od warstwy fizycznej ESP32.
//
// Uklad bufora (jak w UnilinkBus): buf[0]=RAD, buf[1]=TAD, buf[2]=CMD1, ...
// Dlugosc calej ramki = UnilinkFrame::lengthFromCmd1(CMD1) -> 6 / 11 / 16.
// =============================================================================

namespace UnilinkParserModel {

// Wyodrebnia DOKLADNIE jedna ramke z poczatku strumienia wg granicy z CMD1,
// odwzorowujac kryterium podstawowe UnilinkBus::readFrame (bez ciszy).
//
//   buf     — strumien zlozonych bajtow (>= 0 bajtow),
//   count   — liczba dostepnych bajtow w buf,
//   out     — bufor wyjsciowy na pojedyncza ramke,
//   maxLen  — pojemnosc out.
//
// Zwraca liczbe bajtow wyodrebnionej ramki (6/11/16) gdy w buforze jest >= 3
// bajty i zebrano juz pelna dlugosc wyznaczona przez CMD1; w przeciwnym razie 0
// (brak kompletnej ramki). Nadwyzka bajtow (poczatek kolejnej ramki) NIE jest
// kopiowana — to zadanie wywolujacego, by przesunac wskaznik o zwrocona dlugosc.
int parseNextFrame(const uint8_t* buf, int count, uint8_t* out, int maxLen);

} // namespace UnilinkParserModel

#endif // UNILINK_PARSER_MODEL_H
