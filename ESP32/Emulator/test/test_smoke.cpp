// ============================================================================
// Smoke test for the host-side property-based test pipeline.
//
// Spec: unilink-kompendium-alignment, task 1.1
//
// This test does NOT validate any emulator behaviour yet. Its sole purpose is
// to prove that the native (non-Arduino) build + RapidCheck + CTest pipeline
// compiles, links and runs, executing >= 100 iterations per property.
//
// It also compiles a tiny "pure" function (no Arduino.h, only <stdint.h>) to
// demonstrate that the same style of hardware-free logic the real protocol
// modules will use (UnilinkFrame, etc.) builds natively on the host.
// ============================================================================
#include <algorithm>
#include <cstdint>
#include <vector>

#include <rapidcheck.h>

namespace {

// Stand-in pure logic: a hardware-free helper of the kind the real protocol
// modules (UnilinkFrame::parity1, etc.) will be. Depends only on <stdint.h>.
uint8_t sumMod256(const std::vector<uint8_t>& bytes) {
    uint32_t acc = 0;
    for (uint8_t b : bytes) {
        acc += b;
    }
    return static_cast<uint8_t>(acc % 256u);
}

}  // namespace

int main() {
    bool allPassed = true;

    // Property A: reversing a list twice yields the original list.
    // Exercises RapidCheck's generators/shrinking and the run loop.
    allPassed &= rc::check(
        "smoke: reversing a vector twice is the identity",
        [](const std::vector<int>& xs) {
            std::vector<int> twice(xs);
            std::reverse(twice.begin(), twice.end());
            std::reverse(twice.begin(), twice.end());
            RC_ASSERT(twice == xs);
        });

    // Property B: appending a byte increases the mod-256 checksum by that byte
    // (mod 256). Confirms the pure host-side helper compiles and behaves.
    allPassed &= rc::check(
        "smoke: sumMod256 is additive (mod 256) when appending a byte",
        [](const std::vector<uint8_t>& bytes, uint8_t extra) {
            std::vector<uint8_t> extended(bytes);
            extended.push_back(extra);
            const uint8_t expected =
                static_cast<uint8_t>((sumMod256(bytes) + extra) % 256u);
            RC_ASSERT(sumMod256(extended) == expected);
        });

    return allPassed ? 0 : 1;
}
