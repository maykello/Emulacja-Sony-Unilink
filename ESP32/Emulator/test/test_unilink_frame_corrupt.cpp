// ============================================================================
// Property test: walidator odrzuca przeklamane ramki UniLink.
//
// Feature: unilink-kompendium-alignment, Property 2: Zmiana bajtu naglowka/danych/konca daje wynik != Ok (BadParity1/BadParity2/BadEnd)
// Validates: Requirements 2.1, 2.2, 2.3
//
// Tlo: UnilinkFrame::validate jest czysta funkcja (bez Arduino.h), wiec mozna
// ja przetestowac property-based natywnie na hoscie. Strategia testu: zbuduj
// poprawna ramke (short/middle/long) z policzonymi Parity1/Parity2 i bajtem
// koncowym 0, a nastepnie zmien DOKLADNIE jeden bajt na wartosc rozna od
// oryginalnej. Po takiej mutacji validate() musi zwrocic wynik rozny od Ok.
//
// Dodatkowo asercja wskazuje konkretny kod bledu wg pozycji zmienionego bajtu:
//   * bajt naglowka/Parity1 (indeks 0..4)  -> BadParity1  (R2.1, R2.2)
//   * bajt danych/Parity2  (indeks 5..L-2) -> BadParity2  (R2.1, R2.3)  [middle/long]
//   * bajt koncowy         (indeks L-1)    -> BadEnd
// ============================================================================
#include <cstdint>

#include <rapidcheck.h>

#include "UnilinkFrame.h"

namespace {

using UnilinkFrame::ValidateResult;

// Zwraca dlugosc ramki (6/11/16) dla wyboru klasy rozmiaru 0/1/2.
int lenForChoice(int choice) {
    if (choice == 0) return 6;    // short
    if (choice == 1) return 11;   // middle
    return 16;                    // long
}

}  // namespace

int main() {
    bool allPassed = true;

    // Property 2: dla dowolnej poprawnej ramki zmiana pojedynczego bajtu
    // (naglowek/dane/koniec) na inna wartosc daje validate() != Ok, z kodem
    // bledu zgodnym z pozycja zmienionego bajtu.
    allPassed &= rc::check(
        "Property 2: pojedyncza mutacja poprawnej ramki daje validate != Ok",
        []() {
            // --- 1. Wybierz klase rozmiaru i zbuduj POPRAWNA ramke. ---
            const int sizeChoice = *rc::gen::inRange(0, 3);  // 0/1/2
            const int len = lenForChoice(sizeChoice);

            uint8_t frame[16] = {0};

            // Naglowek: RAD, TAD, CMD1, CMD2 z pelnego zakresu bajtu.
            frame[0] = *rc::gen::cast<uint8_t>(rc::gen::inRange(0, 256));  // RAD
            frame[1] = *rc::gen::cast<uint8_t>(rc::gen::inRange(0, 256));  // TAD
            frame[2] = *rc::gen::cast<uint8_t>(rc::gen::inRange(0, 256));  // CMD1
            frame[3] = *rc::gen::cast<uint8_t>(rc::gen::inRange(0, 256));  // CMD2

            const uint8_t p1 =
                UnilinkFrame::parity1(frame[0], frame[1], frame[2], frame[3]);
            frame[4] = p1;

            // Dane (tylko middle/long): middle -> 4 bajty, long -> 9 bajtow.
            const int dataLen = (len == 6) ? 0 : (len - 7);
            for (int i = 0; i < dataLen; ++i) {
                frame[5 + i] =
                    *rc::gen::cast<uint8_t>(rc::gen::inRange(0, 256));
            }

            if (len > 6) {
                // Parity2 = (Parity1 + suma bajtow danych) mod 256.
                frame[len - 2] =
                    UnilinkFrame::parity2(p1, &frame[5], dataLen);
            }
            frame[len - 1] = 0x00;  // bajt koncowy

            // Warunek wstepny: zbudowana ramka jest poprawna.
            RC_PRE(UnilinkFrame::validate(frame, len) == ValidateResult::Ok);

            // --- 2. Zmien DOKLADNIE jeden bajt na inna wartosc. ---
            const int idx = *rc::gen::inRange(0, len);
            const uint8_t orig = frame[idx];
            const uint8_t newVal = *rc::gen::suchThat(
                rc::gen::cast<uint8_t>(rc::gen::inRange(0, 256)),
                [orig](uint8_t v) { return v != orig; });
            frame[idx] = newVal;

            // --- 3. Walidacja musi odrzucic przeklamana ramke. ---
            const ValidateResult result = UnilinkFrame::validate(frame, len);
            RC_ASSERT(result != ValidateResult::Ok);

            // --- 4. Konkretny kod bledu wg pozycji zmienionego bajtu. ---
            ValidateResult expected;
            if (idx <= 4) {
                // RAD/TAD/CMD1/CMD2 wchodza do Parity1; sam Parity1 to indeks 4.
                expected = ValidateResult::BadParity1;  // R2.1, R2.2
            } else if (idx == len - 1) {
                expected = ValidateResult::BadEnd;       // niezerowy bajt konca
            } else {
                // Indeksy 5..L-2 (dane oraz bajt Parity2) — tylko middle/long.
                expected = ValidateResult::BadParity2;   // R2.1, R2.3
            }
            RC_ASSERT(result == expected);
        });

    return allPassed ? 0 : 1;
}
