// ============================================================================
// Property test: dlugosc ramki UniLink wyznaczona przez CMD1.
//
// Feature: unilink-kompendium-alignment, Property 3: Dla dowolnej wartosci CMD1 0x00-0xFF lengthFromCmd1 zwraca 6 (<0x80), 11 (0x80..0xBF), 16 (>=0xC0)
// Validates: Requirements 3.1, 3.2, 3.3
//
// Tlo: UnilinkFrame::lengthFromCmd1 jest czysta funkcja (bez Arduino.h),
// wiec mozna ja przetestowac property-based natywnie na hoscie. CMD1 wyznacza
// granice ramki: short (6) gdy CMD1 < 0x80, middle (11) gdy 0x80 <= CMD1 < 0xC0,
// long (16) gdy CMD1 >= 0xC0.
// ============================================================================
#include <cstdint>

#include <rapidcheck.h>

#include "UnilinkFrame.h"

int main() {
    bool allPassed = true;

    // Property 3: dla kazdej wartosci CMD1 z pelnego zakresu 0x00-0xFF
    // lengthFromCmd1 zwraca dokladnie 6 / 11 / 16 wg progow z Kompendium §3.
    allPassed &= rc::check(
        "Property 3: lengthFromCmd1 zwraca 6/11/16 wg progow CMD1",
        []() {
            // Generuj CMD1 z pelnego zakresu bajtu 0x00-0xFF.
            const uint8_t cmd1 =
                *rc::gen::cast<uint8_t>(rc::gen::inRange(0, 256));

            const int len = UnilinkFrame::lengthFromCmd1(cmd1);

            // Oczekiwana dlugosc wyznaczona niezaleznie od implementacji.
            int expected;
            if (cmd1 < 0x80) {
                expected = 6;            // R3.1: short
            } else if (cmd1 < 0xC0) {    // 0x80..0xBF
                expected = 11;           // R3.2: middle
            } else {                     // 0xC0..0xFF
                expected = 16;           // R3.3: long
            }

            RC_ASSERT(len == expected);
        });

    return allPassed ? 0 : 1;
}
