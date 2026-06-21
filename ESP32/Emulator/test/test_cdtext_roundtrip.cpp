// ============================================================================
// Property test: round-trip CD-TEXT (podzial na pola i ponowne zlozenie).
//
// Feature: unilink-kompendium-alignment, Property 8: Dla nazwy ASCII 0x20–0x7E podział na pola (warianty 0xC9/0xD9 oraz 0xD2) i ponowne złożenie odtwarza tę samą nazwę
// Validates: Requirements 6.7, 6.8
//
// Tlo: CdText::buildField8 / buildFieldD2 dziela nazwe na pola o stalej
// szerokosci (8 dla wariantu 0xC9/0xD9 wg §10.2, 6 dla wariantu 0xD2 wg §10.3),
// a CdText::reassemble sklada kolejne pola z powrotem w nazwe. Funkcje sa czyste
// (bez Arduino.h), wiec testujemy je property-based natywnie na hoscie.
//
// Wlasnosc: dla dowolnej nazwy zlozonej wylacznie z drukowalnych znakow ASCII
// (0x20..0x7E) o dlugosci miszczacej sie w (MAX_FIELD+1) polach — czyli do
// 6*8=48 znakow dla wariantu 8-znakowego i do 6*6=36 dla wariantu 0xD2 —
// reassemble odtwarza dokladnie te sama nazwe (sanitize jest tozsamoscia, bo
// wejscie juz nalezy do 0x20..0x7E).
// ============================================================================
#include <cstddef>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "CdText.h"

namespace {

// Generator pojedynczego drukowalnego znaku ASCII (0x20..0x7E).
rc::Gen<char> genPrintableAscii() {
    return rc::gen::cast<char>(rc::gen::inRange(0x20, 0x7F));
}

// Generator nazwy: lancuch drukowalnych znakow ASCII o dlugosci 0..maxLen.
rc::Gen<std::string> genName(std::size_t maxLen) {
    return rc::gen::map(
        rc::gen::container<std::string>(genPrintableAscii()),
        [maxLen](std::string s) {
            if (s.size() > maxLen) {
                s.resize(maxLen);
            }
            return s;
        });
}

}  // namespace

int main() {
    bool allPassed = true;

    // --- Wariant 8-znakowy (0xC9/0xD9, §10.2): do 6*8 = 48 znakow ---
    allPassed &= rc::check(
        "Property 8: round-trip CD-TEXT wariant 8-znakowy (0xC9/0xD9)",
        []() {
            const std::string name = *genName(CdText::FIELD8_CHARS *
                                              (CdText::MAX_FIELD + 1));
            char out[64] = {0};
            const std::size_t n = CdText::reassemble(
                name.c_str(), CdText::FIELD8_CHARS, out, sizeof(out));
            const std::string got(out, n);
            RC_ASSERT(got == name);
        });

    // --- Wariant 0xD2 (§10.3, tryb CD): do 6*6 = 36 znakow ---
    allPassed &= rc::check(
        "Property 8: round-trip CD-TEXT wariant 0xD2 (6-znakowy)",
        []() {
            const std::string name = *genName(CdText::FIELDD2_CHARS *
                                              (CdText::MAX_FIELD + 1));
            char out[64] = {0};
            const std::size_t n = CdText::reassemble(
                name.c_str(), CdText::FIELDD2_CHARS, out, sizeof(out));
            const std::string got(out, n);
            RC_ASSERT(got == name);
        });

    return allPassed ? 0 : 1;
}
