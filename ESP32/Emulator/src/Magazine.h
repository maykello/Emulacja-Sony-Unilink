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
// Uklad danych ramek (CMD2 oraz parzystosci dokleja dyspozytor — task 10.2):
//   0x97 (middle): D1=liczba utworow, D2=min(BCD), D3=sek(BCD), D4=F|nr
//   0xC5/0xD5 (long): D1..D4=unikatowy ID plyty, D5=F|nr, D6..D9=0
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

// Wypelnij DISC_ID_DATA_LEN bajtow danych identyfikatora plyty z podanego
// (zcache'owanego) seeda. Czysta, deterministyczna forma uzywana zarowno przez
// API sprzetowe, jak i testy hostowe.
inline void fillDiscIdData(uint8_t disc, uint32_t id, uint8_t* d) {
    if (d == nullptr) {
        return;
    }
    d[0] = static_cast<uint8_t>((id >> 24) & 0xFF);
    d[1] = static_cast<uint8_t>((id >> 16) & 0xFF);
    d[2] = static_cast<uint8_t>((id >> 8) & 0xFF);
    d[3] = static_cast<uint8_t>(id & 0xFF);
    d[4] = discNumberByte(disc);   // F|nr — dopasowanie CD-TEXT po plycie
    d[5] = 0x00;
    d[6] = 0x00;
    d[7] = 0x00;
    d[8] = 0x00;
}

// Deterministyczne wypelnienie danych disc ID dla plyty (pure, bez cache).
// To samo disc -> te same bajty (Property 13, Wymaganie 7.5).
inline void buildDiscIdData(uint8_t disc, uint8_t* d) {
    fillDiscIdData(disc, computeDiscId(disc), d);
}

// Wypelnij DISC_INFO_DATA_LEN bajtow danych info plyty (ramka 0x97).
// D1=liczba utworow, D2=min(BCD), D3=sek(BCD), D4=F|nr (Kompendium §8.2).
inline void buildDiscInfoData(uint8_t disc, uint8_t trackCount,
                              uint8_t minutes, uint8_t seconds, uint8_t* d) {
    if (d == nullptr) {
        return;
    }
    d[0] = trackCount;
    d[1] = UnilinkFrame::encodeBcd(minutes);
    d[2] = UnilinkFrame::encodeBcd(seconds);
    d[3] = discNumberByte(disc);
}

// --- API POWIAZANE ZE SPRZETEM (zdefiniowane w Magazine.cpp, pyta AudioPlayer) ---

// Mapa obecnosci plyt (ramka 0x95). Bit i = plyta (i+1) obecna, wynika z
// audioGetTrackCount(i+1) > 0 (Wymaganie 7.1).
uint16_t presenceMap();

// Wypelnij dane info plyty (ramka 0x97): liczba utworow + czas (Wymaganie 7.2).
// d musi miec >= DISC_INFO_DATA_LEN bajtow.
void buildDiscInfo(uint8_t disc, uint8_t* d);

// Wypelnij dane identyfikatora plyty (ramka 0xC5/0xD5), stale per plyta i
// cache'owane miedzy zadaniami (Wymaganie 7.3 / 7.5).
// d musi miec >= DISC_ID_DATA_LEN bajtow.
void buildDiscId(uint8_t disc, uint8_t* d);

} // namespace Magazine

#endif // MAGAZINE_H
