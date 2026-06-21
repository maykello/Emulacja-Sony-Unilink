// ============================================================================
// Property test: sanityzacja do drukowalnego ASCII.
//
// Feature: unilink-kompendium-alignment, Property 10: Dla dowolnego wejścia wszystkie wysyłane bajty należą do 0x20–0x7E
// Validates: Requirements 6.6
//
// Tlo: CdText::sanitizeAscii oraz CdText::buildField8 / buildFieldD2 sa czystymi
// funkcjami (bez Arduino.h), wiec mozna je przetestowac property-based natywnie
// na hoscie. Dla DOWOLNEGO wejscia (lacznie z bajtami spoza 0x20..0x7E: znaki
// kontrolne, bajty >=0x80) kazdy bajt na sciezce nadawania (po sanityzacji oraz
// po zlozeniu pola) musi nalezec do drukowalnego ASCII 0x20..0x7E.
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "CdText.h"

namespace {

// Czy bajt nalezy do drukowalnego ASCII (0x20..0x7E)?
inline bool isPrintableAscii(unsigned char c) {
    return c >= 0x20 && c <= 0x7E;
}

// Buduje napis zakonczony NUL-em z dowolnego wektora bajtow. Pierwszy bajt 0x00
// (jezeli wystapi) traktowany jest jak terminator napisu C — tak samo jak na
// produkcyjnej sciezce, ktora operuje na const char* zakonczonym NUL-em.
std::string toCString(const std::vector<uint8_t>& bytes) {
    std::string s;
    s.reserve(bytes.size());
    for (uint8_t b : bytes) {
        if (b == 0) {
            break;  // koniec napisu C
        }
        s.push_back(static_cast<char>(b));
    }
    return s;
}

} // namespace

int main() {
    bool allPassed = true;

    // Property 10: dla dowolnego wejscia sanitizeAscii zwraca wylacznie bajty
    // z drukowalnego ASCII 0x20..0x7E (R6.6).
    allPassed &= rc::check(
        "Property 10: sanitizeAscii zwraca tylko bajty 0x20..0x7E",
        [](const std::vector<uint8_t>& rawIn) {
            // Generuj wejscie jako dowolny ciag bajtow (lacznie z bajtami spoza
            // 0x20..0x7E), zlozony w napis C zakonczony NUL-em.
            const std::string in = toCString(rawIn);

            // Bufor wyjsciowy z miejscem na terminator.
            std::vector<char> out(in.size() + 1, '\0');
            const size_t n = CdText::sanitizeAscii(in.c_str(), out.data(), out.size());

            // Liczba zapisanych znakow nie moze przekroczyc rozmiaru wejscia.
            RC_ASSERT(n < out.size());

            // Kazdy zapisany bajt musi byc drukowalnym ASCII.
            for (size_t i = 0; i < n; ++i) {
                const unsigned char c = static_cast<unsigned char>(out[i]);
                RC_ASSERT(isPrintableAscii(c));
            }
            // Wynik zawsze zakonczony NUL-em.
            RC_ASSERT(out[n] == '\0');
        });

    // Property 10 (sciezka pol): buildField8 emituje wylacznie bajty 0x20..0x7E
    // dla dowolnej nazwy i dowolnego numeru pola (non-printable -> spacja).
    allPassed &= rc::check(
        "Property 10: buildField8 emituje tylko bajty 0x20..0x7E",
        [](const std::vector<uint8_t>& rawName) {
            const std::string name = toCString(rawName);
            const int field =
                *rc::gen::inRange(0, CdText::MAX_FIELD + 1);  // 0..MAX_FIELD

            uint8_t outChars[CdText::FIELD8_CHARS];
            const int count = CdText::buildField8(name.c_str(), field, outChars);

            RC_ASSERT(count >= 0 && count <= CdText::FIELD8_CHARS);
            // Wszystkie 8 pozycji bufora (zarowno znaki nazwy, jak i dopelnienie)
            // musza byc drukowalnym ASCII.
            for (int i = 0; i < CdText::FIELD8_CHARS; ++i) {
                RC_ASSERT(isPrintableAscii(outChars[i]));
            }
        });

    // Property 10 (sciezka pol 0xD2): buildFieldD2 emituje wylacznie 0x20..0x7E.
    allPassed &= rc::check(
        "Property 10: buildFieldD2 emituje tylko bajty 0x20..0x7E",
        [](const std::vector<uint8_t>& rawName) {
            const std::string name = toCString(rawName);
            const int field =
                *rc::gen::inRange(0, CdText::MAX_FIELD + 1);  // 0..MAX_FIELD

            uint8_t outChars[CdText::FIELDD2_CHARS];
            const int count = CdText::buildFieldD2(name.c_str(), field, outChars);

            RC_ASSERT(count >= 0 && count <= CdText::FIELDD2_CHARS);
            for (int i = 0; i < CdText::FIELDD2_CHARS; ++i) {
                RC_ASSERT(isPrintableAscii(outChars[i]));
            }
        });

    return allPassed ? 0 : 1;
}
