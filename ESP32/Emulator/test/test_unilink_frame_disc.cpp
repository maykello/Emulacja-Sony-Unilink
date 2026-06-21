// ============================================================================
// Property test: round-trip kodowania numeru plyty UniLink (F|nr).
//
// Feature: unilink-kompendium-alignment, Property 11: Dla plyty 1-9 encodeDiscNibble daje 0xF1-0xF9, a discNibbleToNumber jest jego odwrotnoscia
// Validates: Requirements 7.4
//
// Tlo: UnilinkFrame::encodeDiscNibble/discNibbleToNumber sa czystymi funkcjami
// (bez Arduino.h), wiec mozna je przetestowac property-based natywnie na hoscie.
// Numer plyty 1..9 jest kodowany jako gorny nibble F + numer w dolnym nibblu:
// 1 -> 0xF1 ... 9 -> 0xF9. discNibbleToNumber odwraca to kodowanie.
// ============================================================================
#include <cstdint>

#include <rapidcheck.h>

#include "UnilinkFrame.h"

int main() {
    bool allPassed = true;

    // Property 11: dla kazdej plyty 1..9 encodeDiscNibble daje 0xF0|nr
    // (czyli 0xF1..0xF9), a discNibbleToNumber jest jego odwrotnoscia.
    allPassed &= rc::check(
        "Property 11: round-trip kodowania numeru plyty F|nr",
        []() {
            // Generuj numer plyty z zakresu 1..9 (inRange jest poltwarte:
            // 1 wlacznie, 10 wylacznie).
            const uint8_t disc =
                *rc::gen::cast<uint8_t>(rc::gen::inRange(1, 10));

            const uint8_t encoded = UnilinkFrame::encodeDiscNibble(disc);

            // R7.4: gorny nibble F, numer w dolnym nibblu -> 0xF1..0xF9.
            RC_ASSERT(encoded == static_cast<uint8_t>(0xF0 | disc));

            // R7.4: discNibbleToNumber jest odwrotnoscia encodeDiscNibble.
            RC_ASSERT(UnilinkFrame::discNibbleToNumber(encoded) == disc);
        });

    return allPassed ? 0 : 1;
}
