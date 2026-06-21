#ifndef ADDRESS_MANAGER_H
#define ADDRESS_MANAGER_H

#include <stdint.h>
#include <stddef.h>

// =============================================================================
// AddressManager (Menedzer_Adresow) — czysta logika dynamicznego adresowania
// =============================================================================
// Modul CELOWO nie zalezy od Arduino.h, ISR ani globalnego stanu sprzetu —
// tylko od <stdint.h>/<stddef.h>. Dzieki temu kompiluje sie zarowno na ESP32
// (w szkicu Arduino, wolany przez UnilinkProtocol), jak i natywnie na hoscie
// (g++/clang) na potrzeby testow property-based (Property 5/6, taski 5.3/5.4).
//
// Reguly zgodne z Kompendium §6.2 / §12.3 (Wymaganie 4):
//   | Zdarzenie (RAD/CMD1/CMD2)                  | Reakcja                       |
//   |-------------------------------------------|-------------------------------|
//   | start / onBusOff / reset                  | myAddr=0x30, allocated=false  | R4.1,R4.4,R4.5
//   | RAD=0x18, 0x01 0x02 (Anyone) i !allocated | wyslij device info 0x8C       | R4.2
//   | CMD1=0x02, (RAD & 0xF0)==0x30 (Appoint)   | myAddr=RAD, allocated=true,   | R4.3
//   |                                           |   potwierdz 0x8C              |
//   | RAD=0x18, 0x01 0x00 (Bus reset)           | myAddr=0x30, allocated=false  | R4.4
//
// Logika jest CZYSTA (bezstanowe funkcje na strukturze State) — wlasciciel
// stanu (UnilinkProtocol) trzyma instancje State i jedynie przepuszcza zdarzenia
// przez `apply`. Dzieki temu te same reguly testujemy property-based bez sprzetu.
// =============================================================================

namespace AddressManager {

// Adres grupowy CD bez przydzielonego ID. Wartosc rowna Config.h::ADDR_GROUP_CD
// (0x30), ale zdefiniowana lokalnie, by modul pozostal wolny od Arduino.h.
// [DEVIATION §5/§6 (R12)] Start z 0x30 (grupa CD, brak ID) zamiast sztywnego 0x31.
constexpr uint8_t ADDR_GROUP_CD = 0x30;

// Stan sesji adresowej.
struct State {
    uint8_t myAddr;     // 0x30 (start/reset) lub przydzielone ID z grupy 0x3X
    bool    allocated;  // czy radio przydzielilo nam ID (Appoint)
};

// Zdarzenia rozpoznawane przez menedzer adresow.
enum class Event : uint8_t {
    None,      // ramka nieadresowa — bez zmiany stanu
    Start,     // start / onBusOff / timeout / reset cyklu
    Anyone,    // RAD=0x18, 0x01 0x02 — broadcast discovery
    Appoint,   // CMD1=0x02, (RAD & 0xF0)==0x30 — przydzial adresu z grupy CD
    BusReset,  // RAD=0x18, 0x01 0x00 — system reset magistrali
};

// Stan poczatkowy: adres grupowy bez ID (R4.1, R4.5).
inline State initial() {
    return State{ ADDR_GROUP_CD, false };
}

// Czy RAD nalezy do grupy CD (0x30..0x3F)? Adoptujemy KAZDE ID z tej grupy,
// nie tylko 0x31..0x3A (Kompendium §6.2, R4.3).
inline bool isCdGroup(uint8_t rad) {
    return (rad & 0xF0) == ADDR_GROUP_CD;
}

// Klasyfikuje pola ramki do zdarzenia menedzera adresow. Zwraca Event::None dla
// ramek, ktore nie dotycza adresowania (dyspozytor obsluguje je osobno).
// Uwaga: Event::Start nie jest wyprowadzany z ramki — to zdarzenie cyklu zycia
// (begin/onBusOff/timeout), zglaszane jawnie przez wlasciciela stanu.
inline Event classify(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2) {
    constexpr uint8_t BROADCAST = 0x18;
    constexpr uint8_t MASTER    = 0x10;
    if (rad == BROADCAST && tad == MASTER && cmd1 == 0x01 && cmd2 == 0x02) {
        return Event::Anyone;
    }
    if (rad == BROADCAST && tad == MASTER && cmd1 == 0x01 && cmd2 == 0x00) {
        return Event::BusReset;
    }
    if (cmd1 == 0x02 && isCdGroup(rad)) {
        return Event::Appoint;
    }
    return Event::None;
}

// Czysta funkcja przejscia: dla biezacego stanu i zdarzenia zwraca nowy stan.
// `rad` jest uzywany wylacznie dla Event::Appoint (adoptowane ID).
inline State apply(const State& s, Event ev, uint8_t rad) {
    switch (ev) {
        case Event::Start:                       // R4.1, R4.5
        case Event::BusReset:                    // R4.4
            return initial();                    // -> {0x30, false}
        case Event::Appoint:                     // R4.3
            if (isCdGroup(rad)) {
                return State{ rad, true };       // adopcja DOWOLNEGO ID z 0x3X
            }
            return s;                            // poza grupa CD — bez zmiany
        case Event::Anyone:                      // R4.2 — tylko anons, bez zmiany stanu
        case Event::None:
        default:
            return s;
    }
}

// Czy w reakcji na zdarzenie nalezy wyslac device info 0x8C? Tylko na Anyone i
// tylko gdy nie mamy jeszcze ID (R4.2) — inaczej radio przydziel_iloby nam drugi
// adres, biorac nas za nowe urzadzenie.
inline bool shouldSendDeviceInfo(const State& s, Event ev) {
    return ev == Event::Anyone && !s.allocated;
}

} // namespace AddressManager

#endif // ADDRESS_MANAGER_H
