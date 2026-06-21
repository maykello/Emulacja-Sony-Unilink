// ============================================================================
// Property test: determinizm identyfikatora plyty (disc ID).
//
// Feature: unilink-kompendium-alignment, Property 13: Wielokrotne buildDiscId dla tej samej plyty zwraca ten sam identyfikator
// Validates: Requirements 7.5
//
// Tlo: Magazine::computeDiscId i Magazine::buildDiscIdData sa CZYSTYMI,
// inline'owymi helperami (bez Arduino.h, bez AudioPlayer, bez cache), wiec
// mozna je przetestowac property-based natywnie na hoscie. Wymaganie 7.5:
// identyfikator plyty jest STALY/deterministyczny — to samo wejscie (numer
// plyty) zawsze daje ten sam identyfikator i te same bajty danych.
// ============================================================================
#include <cstdint>

#include <rapidcheck.h>

#include "Magazine.h"

int main() {
    bool allPassed = true;

    // Property 13: dla kazdej plyty 0..255 wielokrotne wywolania computeDiscId
    // oraz buildDiscIdData zwracaja identyczny identyfikator / bajty danych.
    allPassed &= rc::check(
        "Property 13: determinizm identyfikatora plyty (disc ID)",
        []() {
            const uint8_t disc = *rc::gen::arbitrary<uint8_t>();

            // R7.5: computeDiscId jest deterministyczne — wielokrotne wywolania
            // dla tej samej plyty zwracaja ten sam seed.
            const uint32_t id1 = Magazine::computeDiscId(disc);
            const uint32_t id2 = Magazine::computeDiscId(disc);
            const uint32_t id3 = Magazine::computeDiscId(disc);
            RC_ASSERT(id1 == id2);
            RC_ASSERT(id2 == id3);

            // R7.5: buildDiscIdData wypelnia te same DISC_ID_DATA_LEN bajtow przy
            // kazdym wywolaniu dla tej samej plyty.
            uint8_t d1[Magazine::DISC_ID_DATA_LEN];
            uint8_t d2[Magazine::DISC_ID_DATA_LEN];
            Magazine::buildDiscIdData(disc, d1);
            Magazine::buildDiscIdData(disc, d2);
            for (int i = 0; i < Magazine::DISC_ID_DATA_LEN; ++i) {
                RC_ASSERT(d1[i] == d2[i]);
            }
        });

    return allPassed ? 0 : 1;
}
