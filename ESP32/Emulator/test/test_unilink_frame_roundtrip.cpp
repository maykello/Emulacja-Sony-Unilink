// ============================================================================
// Property test: round-trip walidacji sum kontrolnych nadawanych ramek UniLink.
//
// Feature: unilink-kompendium-alignment, Property 1: Ramka zbudowana z policzonymi Parity1/Parity2 i bajtem końcowym 0 przechodzi validate z wynikiem Ok
// Validates: Requirements 2.1, 2.4
//
// Tlo: UnilinkFrame::parity1/parity2/validate sa czystymi funkcjami (bez
// Arduino.h), wiec mozna je testowac property-based natywnie na hoscie. Ta
// wlasnosc sprawdza, ze KAZDA ramka zbudowana tak, jak robi to warstwa
// nadawcza — z policzonymi sumami kontrolnymi Parity1 (oraz Parity2 dla
// middle/long) i bajtem koncowym 0x00 — przechodzi nasza wlasna walidacje z
// wynikiem Ok. To gwarantuje spojnosc nadawania i odbioru (R2.1, R2.4).
//
// Uklady ramek (Kompendium §4):
//   short  (6B):  RAD TAD CMD1 CMD2 P1 0
//   middle (11B): RAD TAD CMD1 CMD2 P1 D1 D2 D3 D4 P2 0
//   long   (16B): RAD TAD CMD1 CMD2 P1 D1..D9 P2 0
// ============================================================================
#include <cstdint>
#include <vector>

#include <rapidcheck.h>

#include "UnilinkFrame.h"

namespace {

// Generator pojedynczego bajtu z pelnego zakresu 0x00-0xFF.
rc::Gen<uint8_t> genByte() {
    return rc::gen::cast<uint8_t>(rc::gen::inRange(0, 256));
}

// Buduje kompletna ramke (z policzonymi parzystosciami i bajtem koncowym 0)
// dla podanej liczby bajtow danych (0 = short, 4 = middle, 9 = long), tak jak
// robia to funkcje nadawcze UnilinkBus::sendShort/sendMedium/sendLong.
std::vector<uint8_t> buildFrame(uint8_t rad, uint8_t tad, uint8_t cmd1,
                                uint8_t cmd2,
                                const std::vector<uint8_t>& data) {
    std::vector<uint8_t> frame;
    frame.push_back(rad);
    frame.push_back(tad);
    frame.push_back(cmd1);
    frame.push_back(cmd2);

    const uint8_t p1 = UnilinkFrame::parity1(rad, tad, cmd1, cmd2);
    frame.push_back(p1);

    if (data.empty()) {
        // Short: brak danych, brak Parity2.
        frame.push_back(0x00);  // bajt koncowy
        return frame;
    }

    for (uint8_t b : data) {
        frame.push_back(b);
    }
    const uint8_t p2 = UnilinkFrame::parity2(
        p1, data.data(), static_cast<int>(data.size()));
    frame.push_back(p2);
    frame.push_back(0x00);  // bajt koncowy
    return frame;
}

}  // namespace

int main() {
    bool allPassed = true;

    // --- Short (6B): tylko Parity1 + bajt koncowy 0 (R2.4) ---
    allPassed &= rc::check(
        "Property 1: round-trip validate Ok dla ramki short (6B)",
        []() {
            const uint8_t rad = *genByte();
            const uint8_t tad = *genByte();
            const uint8_t cmd1 = *genByte();
            const uint8_t cmd2 = *genByte();

            std::vector<uint8_t> frame = buildFrame(rad, tad, cmd1, cmd2, {});
            RC_ASSERT(frame.size() == 6u);
            RC_ASSERT(UnilinkFrame::validate(frame.data(),
                                             static_cast<int>(frame.size())) ==
                      UnilinkFrame::ValidateResult::Ok);
        });

    // --- Middle (11B): Parity1 + Parity2 + bajt koncowy 0 (R2.1) ---
    allPassed &= rc::check(
        "Property 1: round-trip validate Ok dla ramki middle (11B)",
        []() {
            const uint8_t rad = *genByte();
            const uint8_t tad = *genByte();
            const uint8_t cmd1 = *genByte();
            const uint8_t cmd2 = *genByte();
            std::vector<uint8_t> data;
            for (int i = 0; i < 4; ++i) {
                data.push_back(*genByte());
            }

            std::vector<uint8_t> frame = buildFrame(rad, tad, cmd1, cmd2, data);
            RC_ASSERT(frame.size() == 11u);
            RC_ASSERT(UnilinkFrame::validate(frame.data(),
                                             static_cast<int>(frame.size())) ==
                      UnilinkFrame::ValidateResult::Ok);
        });

    // --- Long (16B): Parity1 + Parity2 + bajt koncowy 0 (R2.1) ---
    allPassed &= rc::check(
        "Property 1: round-trip validate Ok dla ramki long (16B)",
        []() {
            const uint8_t rad = *genByte();
            const uint8_t tad = *genByte();
            const uint8_t cmd1 = *genByte();
            const uint8_t cmd2 = *genByte();
            std::vector<uint8_t> data;
            for (int i = 0; i < 9; ++i) {
                data.push_back(*genByte());
            }

            std::vector<uint8_t> frame = buildFrame(rad, tad, cmd1, cmd2, data);
            RC_ASSERT(frame.size() == 16u);
            RC_ASSERT(UnilinkFrame::validate(frame.data(),
                                             static_cast<int>(frame.size())) ==
                      UnilinkFrame::ValidateResult::Ok);
        });

    return allPassed ? 0 : 1;
}
