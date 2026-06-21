// ============================================================================
// Property test: wybor komendy CD-TEXT (CMD1) wg numeru pola.
//
// Feature: unilink-kompendium-alignment, Property 9: Pola 0–1 używają 0xC9 (utwór)/0xCD (płyta), pola 2–5 używają 0xD9/0xDD
// Validates: Requirements 6.3, 6.4
//
// Tlo: CdText::commandForField jest czysta funkcja (bez Arduino.h), wiec mozna
// ja testowac property-based natywnie na hoscie. Numer pola wyznacza komende
// CMD1: pola 0–1 -> 0xC9 (utwor)/0xCD (plyta), pola 2–5 -> 0xD9 (utwor)/0xDD
// (plyta). Flaga isDisc wybiera wariant plyty vs utworu (Kompendium §10.2).
// ============================================================================
#include <rapidcheck.h>

#include "CdText.h"

int main() {
    bool allPassed = true;

    // Property 9: dla dowolnego pola 0..5 i dowolnej flagi isDisc komenda CMD1
    // odpowiada progowi pola (0–1 niskie, 2–5 wysokie) oraz wariantowi
    // utwor/plyta.
    allPassed &= rc::check(
        "Property 9: commandForField zwraca 0xC9/0xCD dla pol 0-1 i 0xD9/0xDD dla pol 2-5",
        []() {
            // Pole w zakresie 0..5 (inRange jest polotwarty: [0, 6)).
            const int field = *rc::gen::inRange(0, 6);
            const bool isDisc = *rc::gen::arbitrary<bool>();

            const uint8_t cmd = CdText::commandForField(field, isDisc);

            // Oczekiwana komenda wyznaczona niezaleznie od implementacji.
            uint8_t expected;
            if (field <= 1) {
                expected = isDisc ? 0xCD : 0xC9;  // R6.3/R6.4: pola 0–1
            } else {
                expected = isDisc ? 0xDD : 0xD9;  // R6.3/R6.4: pola 2–5
            }

            RC_ASSERT(cmd == expected);
        });

    return allPassed ? 0 : 1;
}
