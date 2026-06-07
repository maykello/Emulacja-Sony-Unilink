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

// Harmonogram Slave Break — wywolywac w kazdej iteracji loop() (gdy BUS=1).
void serviceSlaveBreak(bool busPowered);

// Sprawdza czy radio zniklo (brak PINGa > RADIO_TIMEOUT_MS). Jesli tak —
// resetuje stan sesji i zwraca true (raz). Wywolywac w loop().
bool serviceTimeout(unsigned long now);

// Reset sesji przy zaniku zasilania magistrali (BUS=0).
void onBusOff();

// Czy radio przydzielilo nam adres (sesja aktywna)?
bool isAllocated();

} // namespace UnilinkProtocol

#endif // UNILINK_PROTOCOL_H
