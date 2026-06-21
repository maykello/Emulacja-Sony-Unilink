// ============================================================================
// Property test: round-trip kodowania BCD czasu/utworu (UnilinkFrame).
//
// Feature: unilink-kompendium-alignment, Property 22: Dla 0–59 encodeBcd/decodeBcd są wzajemnie odwrotne, a encodeBcdFpad dla <10 ustawia górny nibble F
// Validates: Requirements 11.1, 11.2
//
// Tlo: UnilinkFrame::encodeBcd / decodeBcd / encodeBcdFpad to czyste funkcje
// (bez Arduino.h), wiec mozna je przetestowac property-based natywnie na hoscie.
// Pola minut/sekund/utworu w ramce 0x90 koduje sie w BCD; dla wartosci < 10
// wariant "Fpad" ustawia gorny nibble na F (np. 7 -> 0xF7) zgodnie z §11.2.
// ============================================================================
#include <cstdint>

#include <rapidcheck.h>

#include "UnilinkFrame.h"

int main() {
    bool allPassed = true;

    // Property 22: dla dowolnej wartosci 0..59 encodeBcd i decodeBcd sa
    // wzajemnie odwrotne (round-trip), a encodeBcdFpad dla wartosci < 10
    // ustawia gorny nibble na F (0xF0) zachowujac wartosc w dolnym nibblu.
    allPassed &= rc::check(
        "Property 22: round-trip BCD czasu i Fpad dla <10",
        []() {
            // Generuj wartosc z zakresu 0..59 (sekundy/minuty/utwor).
            const uint8_t value =
                static_cast<uint8_t>(*rc::gen::inRange(0, 60));

            // 1) encodeBcd/decodeBcd sa wzajemnie odwrotne na 0..59.
            const uint8_t bcd = UnilinkFrame::encodeBcd(value);
            RC_ASSERT(UnilinkFrame::decodeBcd(bcd) == value);

            // 2) Dla value < 10 encodeBcdFpad ustawia gorny nibble na F
            //    i trzyma wartosc w dolnym nibblu (7 -> 0xF7).
            if (value < 10) {
                const uint8_t fpad = UnilinkFrame::encodeBcdFpad(value);
                RC_ASSERT((fpad & 0xF0) == 0xF0);
                RC_ASSERT((fpad & 0x0F) == value);
            }
        });

    return allPassed ? 0 : 1;
}
