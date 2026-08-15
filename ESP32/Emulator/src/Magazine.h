#ifndef MAGAZINE_H
#define MAGAZINE_H

#include <stdint.h>
#include <stddef.h>
#include "UnilinkFrame.h"

// =============================================================================
// Magazine / Modul_Magazynka — raportowanie obecnosci plyt i identyfikatorow
// =============================================================================
// Modul odpowiada za logike magazynka zmieniarki (Wymaganie 7, Kompendium
// §8.2 / C.3 / C.4):
//   * mapa obecnosci plyt              -> ramka 0x95   (presenceMap)
//   * info plyty (liczba utworow+czas) -> ramka 0x97   (buildDiscInfo)
//   * identyfikator plyty (disc ID)    -> ramka 0xC5/0xD5 (buildDiscId)
//   * kodowanie numeru plyty F|nr      -> discNumberByte
//
// CZYSTA LOGIKA (presenceMapFrom, computeDiscId, fillDiscIdData,
// buildDiscInfoData, discNumberByte) CELOWO nie zalezy od Arduino.h ani od
// AudioPlayer — jest zdefiniowana inline w naglowku, dzieki czemu kompiluje sie
// natywnie na hoscie i daje sie testowac property-based (Property 12 i 13)
// bez sprzetu. Funkcje powiazane ze sprzetem (presenceMap, buildDiscInfo,
// buildDiscId) zyja w Magazine.cpp i odpytuja AudioPlayer.
//
// Uklad danych ramek (CMD2 oraz parzystosci dokleja dyspozytor):
//   0x95 (middle): CMD2 = PRZEPLATANA mapa obecnosci, D1 = plyty 9-10,
//                  D2=D3=0, D4 = numer plyty w gornym nibblu | 0x0A
//   0x97 (middle): CMD2=0x01, D1=liczba utworow (BCD), D2=min(BCD),
//                  D3=sek(BCD), D4=numer plyty w gornym nibblu
//   0xC5/0xD5 (long): CMD2=0xA|setne, D1=liczba utworow (BCD), D2=min (BCD),
//                  D3=sek (BCD), D4=0xF0 (0xC5) / 0xF1 (0xD5), D5..D8=0,
//                  D9 = numer plyty w gornym nibblu | 0x08
// =============================================================================

namespace Magazine {

// Liczba bajtow danych wypelnianych przez buildDiscInfo (ramka 0x97, middle).
constexpr int DISC_INFO_DATA_LEN = 4;
// Liczba bajtow danych wypelnianych przez buildDiscId (ramka 0xC5/0xD5, long).
constexpr int DISC_ID_DATA_LEN = 9;

// --- CZYSTE, TESTOWALNE HELPERY (bez zaleznosci od Arduino/AudioPlayer) ---

// Mapa obecnosci plyt z licznikow utworow. trackCounts[i] = liczba utworow na
// plycie (i+1). Bit i ustawiony WTEDY I TYLKO WTEDY, gdy trackCounts[i] > 0
// (Property 12, Wymaganie 7.1). numDiscs ograniczone do 16 (rozmiar mapy).
inline uint16_t presenceMapFrom(const uint8_t* trackCounts, uint8_t numDiscs) {
    if (trackCounts == nullptr) {
        return 0;
    }
    if (numDiscs > 16) {
        numDiscs = 16;
    }
    uint16_t map = 0;
    for (uint8_t i = 0; i < numDiscs; ++i) {
        if (trackCounts[i] > 0) {
            map = static_cast<uint16_t>(map | (1u << i));
        }
    }
    return map;
}

// --- PRZEPLATANA MAPA OBECNOSCI DLA RAMKI 0x95 ---
// Sony NIE uklada bitow po kolei. Wg tabeli komend UniLink oraz sniffu
// CDX-M670 (`70 31 95 08 3E 00 00 00 8A C8 00` = obecna wylacznie plyta 8):
//   CMD2: bit4=CD1, bit5=CD2, bit6=CD3, bit7=CD4, bit0=CD5, bit1=CD6,
//         bit2=CD7, bit3=CD8
//   D1  : bit4=CD9, bit5=CD10
// Wczesniej wysylalismy zwykla mape liniowa w D1/D2, czego radio nie rozumialo
// (magazynek pokazywal sie jako pusty). `map` to mapa liniowa (bit i = plyta i+1).
inline uint8_t magazineCmd2FromMap(uint16_t map) {
    uint8_t cmd2 = 0;
    if (map & (1u << 0)) cmd2 |= 0x10;   // CD1
    if (map & (1u << 1)) cmd2 |= 0x20;   // CD2
    if (map & (1u << 2)) cmd2 |= 0x40;   // CD3
    if (map & (1u << 3)) cmd2 |= 0x80;   // CD4
    if (map & (1u << 4)) cmd2 |= 0x01;   // CD5
    if (map & (1u << 5)) cmd2 |= 0x02;   // CD6
    if (map & (1u << 6)) cmd2 |= 0x04;   // CD7
    if (map & (1u << 7)) cmd2 |= 0x08;   // CD8
    return cmd2;
}

inline uint8_t magazineD1FromMap(uint16_t map) {
    uint8_t d1 = 0;
    if (map & (1u << 8)) d1 |= 0x10;     // CD9
    if (map & (1u << 9)) d1 |= 0x20;     // CD10
    return d1;
}

// Deterministyczny, STALY identyfikator plyty (seed) dla numeru plyty.
// To samo wejscie zawsze daje to samo wyjscie -> Property 13 (Wymaganie 7.5).
// Wariant FNV-1a-podobny: rozne numery plyt daja rozne identyfikatory.
inline uint32_t computeDiscId(uint8_t disc) {
    uint32_t h = 2166136261u;            // FNV offset basis
    h = (h ^ disc) * 16777619u;          // FNV prime
    h = (h ^ 0x5Au) * 16777619u;         // dodatkowe wymieszanie
    h = (h ^ (disc << 3)) * 16777619u;
    return h;
}

// Kodowanie numeru plyty jako gorny nibble F + numer w dolnym (1 -> 0xF1).
// Cienka nakladka na UnilinkFrame::encodeDiscNibble (Wymaganie 7.4).
inline uint8_t discNumberByte(uint8_t disc) {
    return UnilinkFrame::encodeDiscNibble(disc);
}

// --- SYNTETYCZNY "TOC" PLYTY (calkowity czas) ---
// Prawdziwa zmieniarka podaje w ramkach 0xC5/0xD5/0x97 calkowity czas plyty z
// TOC-a. Emulator gra pliki z pendrive'a i nie zna sumarycznego czasu, wiec
// wyprowadzamy go DETERMINISTYCZNIE z identyfikatora plyty. Dzieki temu radio
// widzi STALY "unikatowy numer plyty" (klucz Custom File) miedzy zadaniami
// (Wymaganie 7.5), a jednoczesnie kazda plyta ma inny.
inline void discIdToc(uint32_t id, uint8_t& minutes, uint8_t& seconds, uint8_t& hundredths) {
    minutes    = static_cast<uint8_t>(10 + (id % 60));          // 10..69 min
    seconds    = static_cast<uint8_t>((id >> 8) % 60);          // 0..59 s
    hundredths = static_cast<uint8_t>((id >> 16) % 10);         // 0..9 (dolny nibbel CMD2)
}

// Wypelnij DISC_ID_DATA_LEN bajtow danych ramki 0xC5/0xD5 oraz jej CMD2.
// Uklad odtworzony 1:1 ze sniffu CDX-M670:
//   10 31 C5 A2 A8 | 24 77 52 F0 00 00 00 00 88 | 0D 00
//   CMD2 = 0xA|setne, D1 = liczba utworow (BCD), D2 = minuty (BCD),
//   D3 = sekundy (BCD), D4 = 0xF0 (0xC5, skan) / 0xF1 (0xD5, zmiana plyty),
//   D5..D8 = 0x00, D9 = numer plyty w gornym nibblu | 0x08.
// Wczesniej wysylalismy tu 4 bajty surowego hasha FNV — radio odrzucalo taka
// ramke jako bezsensowny TOC.
inline void fillDiscIdData(uint8_t disc, uint32_t id, uint8_t trackCount,
                           bool discChangeVariant, uint8_t& cmd2, uint8_t* d) {
    uint8_t minutes = 0, seconds = 0, hundredths = 0;
    discIdToc(id, minutes, seconds, hundredths);
    cmd2 = static_cast<uint8_t>(0xA0 | (hundredths & 0x0F));
    if (d == nullptr) {
        return;
    }
    d[0] = UnilinkFrame::encodeBcd(trackCount);          // D1 = liczba utworow
    d[1] = UnilinkFrame::encodeBcd(minutes);             // D2 = minuty calkowite
    d[2] = UnilinkFrame::encodeBcd(seconds);             // D3 = sekundy calkowite
    d[3] = discChangeVariant ? 0xF1 : 0xF0;              // D4 = staly marker
    d[4] = 0x00;                                         // D5
    d[5] = 0x00;                                         // D6
    d[6] = 0x00;                                         // D7
    d[7] = 0x00;                                         // D8
    d[8] = UnilinkFrame::discHighNibble(disc, 0x08);     // D9 = plyta | brak nazwy
}

// Wypelnij DISC_INFO_DATA_LEN bajtow danych info plyty (ramka 0x97).
// D1 = liczba utworow (BCD), D2 = min (BCD), D3 = sek (BCD),
// D4 = numer plyty w GORNYM nibblu. Wczesniej D1 bylo liczba binarna, a D4
// kodowaniem F|nr — oba niezgodne z tabela komend i sniffem.
inline void buildDiscInfoData(uint8_t disc, uint8_t trackCount,
                              uint8_t minutes, uint8_t seconds, uint8_t* d) {
    if (d == nullptr) {
        return;
    }
    d[0] = UnilinkFrame::encodeBcd(trackCount);
    d[1] = UnilinkFrame::encodeBcd(minutes);
    d[2] = UnilinkFrame::encodeBcd(seconds);
    d[3] = UnilinkFrame::discHighNibble(disc, 0x00);
}

// --- API POWIAZANE ZE SPRZETEM (zdefiniowane w Magazine.cpp, pyta AudioPlayer) ---

// Mapa obecnosci plyt (ramka 0x95). Bit i = plyta (i+1) obecna, wynika z
// audioGetTrackCount(i+1) > 0 (Wymaganie 7.1).
uint16_t presenceMap();

// Wypelnij dane info plyty (ramka 0x97): liczba utworow + czas (Wymaganie 7.2).
// d musi miec >= DISC_INFO_DATA_LEN bajtow.
void buildDiscInfo(uint8_t disc, uint8_t* d);

// Wypelnij dane identyfikatora plyty (ramka 0xC5/0xD5) oraz jej CMD2. Stale per
// plyta i cache'owane miedzy zadaniami (Wymaganie 7.3 / 7.5).
// `discChangeVariant` = true dla 0xD5 (zmiana plyty w trakcie grania),
// false dla 0xC5 (skan magazynka po resecie / wymianie magazynka).
// d musi miec >= DISC_ID_DATA_LEN bajtow.
void buildDiscId(uint8_t disc, bool discChangeVariant, uint8_t& cmd2, uint8_t* d);

} // namespace Magazine

#endif // MAGAZINE_H
