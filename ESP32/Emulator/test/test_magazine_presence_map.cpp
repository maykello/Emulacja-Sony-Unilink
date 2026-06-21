// ============================================================================
// Property test: mapa obecnosci plyt magazynka (ramka 0x95).
//
// Feature: unilink-kompendium-alignment, Property 12: W mapie 0x95 bit plyty jest ustawiony wtedy i tylko wtedy, gdy plyta jest obecna
// Validates: Requirements 7.1
//
// Tlo: Magazine::presenceMapFrom jest czystym, inline'owanym helperem (bez
// Arduino.h ani AudioPlayer), wiec mozna go przetestowac property-based
// natywnie na hoscie. Mapa to 16-bitowe pole, w ktorym bit i odpowiada plycie
// (i+1). Bit i ma byc ustawiony WTEDY I TYLKO WTEDY, gdy trackCounts[i] > 0
// (plyta obecna). numDiscs jest ograniczone do 16 (rozmiar mapy), a bity
// >= numDiscs musza pozostac zerowe.
// ============================================================================
#include <cstdint>
#include <vector>

#include <rapidcheck.h>

#include "Magazine.h"

int main() {
    bool allPassed = true;

    // Property 12: bit i ustawiony IFF trackCounts[i] > 0 dla i < numDiscs,
    // a bity >= numDiscs sa zerowe.
    allPassed &= rc::check(
        "Property 12: mapa obecnosci plyt odzwierciedla dostepne plyty",
        []() {
            // Liczba plyt 0..16 (inRange jest poltwarte: 0 wlacznie,
            // 17 wylacznie -> max 16).
            const uint8_t numDiscs =
                *rc::gen::cast<uint8_t>(rc::gen::inRange(0, 17));

            // Wygeneruj liczniki utworow (0..50) dla kazdej z numDiscs plyt.
            // 0 = plyta nieobecna, > 0 = plyta obecna.
            const std::vector<uint8_t> counts =
                *rc::gen::container<std::vector<uint8_t>>(
                    numDiscs,
                    rc::gen::cast<uint8_t>(rc::gen::inRange(0, 51)));

            const uint16_t map =
                Magazine::presenceMapFrom(counts.data(), numDiscs);

            // R7.1: dla kazdej plyty w zakresie bit IFF licznik > 0.
            for (uint8_t i = 0; i < numDiscs; ++i) {
                const bool bitSet = (map & (1u << i)) != 0;
                const bool present = counts[i] > 0;
                RC_ASSERT(bitSet == present);
            }

            // R7.1: bity poza zakresem numDiscs musza byc zerowe.
            for (uint8_t i = numDiscs; i < 16; ++i) {
                RC_ASSERT((map & (1u << i)) == 0);
            }
        });

    return allPassed ? 0 : 1;
}
