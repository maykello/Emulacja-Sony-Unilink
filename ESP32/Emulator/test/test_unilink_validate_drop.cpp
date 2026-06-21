// ============================================================================
// Unit test: Parity2 validation triggers DROP in handlePacket.
//
// Feature: unilink-kompendium-alignment, Task 4.2: Napisać test jednostkowy
//          rejestracji DROP (validate z błędem Parity2).
//
// Validates: Requirements 2.5.
//
// Tło: W UnilinkProtocol::handlePacket po walidacji ramki przez
// UnilinkFrame::validate() przy wyniku != Ok wywoływane jest
// Diagnostics::recordNote("DROP"). Ten test SKUPIONY jest tylko na walidatorze
// (UnilinkFrame::validate) i potwierdza, że uszkodzony Parity2 daje
// ValidateResult::BadParity2 — co w pełnym dyspozytorze spowoduje DROP.
//
// Ważne: Test host-side (natywny, bez Arduino.h), więc NIE można testować
// Diagnostics::recordNote() bezpośrednio (zależy on od ESP32). Zamiast tego
// asercje:
//   • validate(frame, len) zwraca BadParity2 dla middle/long z uszkodzonym P2
//   • validate(frame, len) zwraca Ok dla tej samej ramki z poprawnym P2
// ============================================================================
#include <cassert>
#include <cstdio>
#include <cstring>

#include "UnilinkFrame.h"

using UnilinkFrame::ValidateResult;

// ============================================================================
// Pomocnik: buduje pełną ramkę middle (11B) lub long (16B) z obliczonymi
// Parity1/Parity2.
// ============================================================================
static void buildFrame(uint8_t* frame, int len,
                       uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2,
                       const uint8_t* data, int dataLen,
                       bool corruptParity2, uint8_t corruptionMask) {
    // Nagłówek
    frame[0] = rad;
    frame[1] = tad;
    frame[2] = cmd1;
    frame[3] = cmd2;

    // Parity1 = (RAD + TAD + CMD1 + CMD2) mod 256
    frame[4] = UnilinkFrame::parity1(rad, tad, cmd1, cmd2);

    // Dane (jeśli są)
    if (dataLen > 0 && data != nullptr) {
        std::memcpy(&frame[5], data, dataLen);
    }

    // Dane zerujemy jeśli nie podano
    if (dataLen > 0 && data == nullptr) {
        std::memset(&frame[5], 0, dataLen);
    }

    // Parity2 = (Parity1 + suma bajtów danych) mod 256
    const uint8_t p2 = UnilinkFrame::parity2(frame[4], &frame[5], dataLen);
    frame[len - 2] = corruptParity2 ? (p2 ^ corruptionMask) : p2;

    // Bajt końcowy
    frame[len - 1] = 0x00;
}

// ============================================================================
// Test 1: middle frame (11B) z uszkodzonym Parity2 -> BadParity2
// ============================================================================
static bool testMiddleFrameCorruptedParity2() {
    uint8_t frame[11];

    // middle: 4 bajty danych (D1..D4)
    const uint8_t data[4] = {0x12, 0x34, 0x56, 0x78};

    // Zbuduj ramkę z POPRAWNYM Parity2
    buildFrame(frame, 11, 0x70, 0x31, 0xC0, 0x00, data, 4,
               /*corrupt=*/false, 0);

    // Walidacja powinna zakończyć się sukcesem
    ValidateResult resultOk = UnilinkFrame::validate(frame, 11);
    if (resultOk != ValidateResult::Ok) {
        std::printf("[FAIL] testMiddleFrameCorruptedParity2: "
                    "poprawna ramka middle zwraca %d, oczekiwano 0\n",
                    static_cast<int>(resultOk));
        return false;
    }

    // Uszkodz Parity2 (xor maską 0xFF — zawsze zmieni wartość)
    buildFrame(frame, 11, 0x70, 0x31, 0xC0, 0x00, data, 4,
               /*corrupt=*/true, 0xFF);

    // Walidacja powinna zwrócić BadParity2
    ValidateResult resultBad = UnilinkFrame::validate(frame, 11);
    if (resultBad != ValidateResult::BadParity2) {
        std::printf("[FAIL] testMiddleFrameCorruptedParity2: "
                    "uszkodzona ramka middle zwraca %d, oczekiwano 3 (BadParity2)\n",
                    static_cast<int>(resultBad));
        return false;
    }

    std::printf("[PASS] testMiddleFrameCorruptedParity2\n");
    return true;
}

// ============================================================================
// Test 2: long frame (16B) z uszkodzonym Parity2 -> BadParity2
// ============================================================================
static bool testLongFrameCorruptedParity2() {
    uint8_t frame[16];

    // long: 9 bajtów danych (D1..D9)
    const uint8_t data[9] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};

    // Zbuduj ramkę z POPRAWNYM Parity2
    buildFrame(frame, 16, 0x70, 0x31, 0xC0, 0x00, data, 9,
               /*corrupt=*/false, 0);

    // Walidacja powinna zakończyć się sukcesem
    ValidateResult resultOk = UnilinkFrame::validate(frame, 16);
    if (resultOk != ValidateResult::Ok) {
        std::printf("[FAIL] testLongFrameCorruptedParity2: "
                    "poprawna ramka long zwraca %d, oczekiwano 0\n",
                    static_cast<int>(resultOk));
        return false;
    }

    // Uszkodz Parity2 (xor maską 0x5A — dowolna niestała)
    buildFrame(frame, 16, 0x70, 0x31, 0xC0, 0x00, data, 9,
               /*corrupt=*/true, 0x5A);

    // Walidacja powinna zwrócić BadParity2
    ValidateResult resultBad = UnilinkFrame::validate(frame, 16);
    if (resultBad != ValidateResult::BadParity2) {
        std::printf("[FAIL] testLongFrameCorruptedParity2: "
                    "uszkodzona ramka long zwraca %d, oczekiwano 3 (BadParity2)\n",
                    static_cast<int>(resultBad));
        return false;
    }

    std::printf("[PASS] testLongFrameCorruptedParity2\n");
    return true;
}

// ============================================================================
// Test 3: short frame (6B) — brak Parity2, więc tylko BadEnd/BadParity1
// ============================================================================
static bool testShortFrameParity2NotApplicable() {
    uint8_t frame[6];

    // Short ma tylko Parity1 (i bajt końcowy), Parity2 nie występuje.
    // Walidator nie sprawdza Parity2 dla short — to będzie BadParity1 albo BadEnd.
    frame[0] = 0x70;  // RAD
    frame[1] = 0x31;  // TAD
    frame[2] = 0x00;  // CMD1 < 0x80 -> short
    frame[3] = 0x00;  // CMD2

    // Zbuduj z POPRAWNYM Parity1 i bajtem końcowym
    frame[4] = UnilinkFrame::parity1(0x70, 0x31, 0x00, 0x00);
    frame[5] = 0x00;

    ValidateResult resultOk = UnilinkFrame::validate(frame, 6);
    if (resultOk != ValidateResult::Ok) {
        std::printf("[FAIL] testShortFrameParity2NotApplicable: "
                    "poprawna ramka short zwraca %d, oczekiwano 0\n",
                    static_cast<int>(resultOk));
        return false;
    }

    // Uszkodz Parity1 (nie Parity2 — short nie ma Parity2)
    frame[4] ^= 0xFF;

    ValidateResult resultBad = UnilinkFrame::validate(frame, 6);
    if (resultBad != ValidateResult::BadParity1) {
        std::printf("[FAIL] testShortFrameParity2NotApplicable: "
                    "uszkodzona ramka short zwraca %d, oczekiwano 2 (BadParity1)\n",
                    static_cast<int>(resultBad));
        return false;
    }

    std::printf("[PASS] testShortFrameParity2NotApplicable\n");
    return true;
}

// ============================================================================
// Test 4: middle frame z POPRAWNYM Parity2 -> Ok
// ============================================================================
static bool testMiddleFrameValidParity2() {
    uint8_t frame[11];

    const uint8_t data[4] = {0xAB, 0xCD, 0xEF, 0x01};

    // Poprawna ramka middle
    buildFrame(frame, 11, 0x10, 0x30, 0x84, 0xD9, data, 4,
               /*corrupt=*/false, 0);

    ValidateResult result = UnilinkFrame::validate(frame, 11);
    if (result != ValidateResult::Ok) {
        std::printf("[FAIL] testMiddleFrameValidParity2: "
                    "poprawna ramka middle zwraca %d, oczekiwano 0\n",
                    static_cast<int>(result));
        return false;
    }

    std::printf("[PASS] testMiddleFrameValidParity2\n");
    return true;
}

// ============================================================================
// Test 5: long frame z POPRAWNYM Parity2 -> Ok
// ============================================================================
static bool testLongFrameValidParity2() {
    uint8_t frame[16];

    const uint8_t data[9] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90};

    // Poprawna ramka long
    buildFrame(frame, 16, 0x70, 0x31, 0xC0, 0x00, data, 9,
               /*corrupt=*/false, 0);

    ValidateResult result = UnilinkFrame::validate(frame, 16);
    if (result != ValidateResult::Ok) {
        std::printf("[FAIL] testLongFrameValidParity2: "
                    "poprawna ramka long zwraca %d, oczekiwano 0\n",
                    static_cast<int>(result));
        return false;
    }

    std::printf("[PASS] testLongFrameValidParity2\n");
    return true;
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::printf("=== Unit test: Parity2 validation (DROP) ===\n\n");

    bool allPassed = true;

    allPassed &= testMiddleFrameCorruptedParity2();
    allPassed &= testLongFrameCorruptedParity2();
    allPassed &= testShortFrameParity2NotApplicable();
    allPassed &= testMiddleFrameValidParity2();
    allPassed &= testLongFrameValidParity2();

    std::printf("\n=== %s ===\n", allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    return allPassed ? 0 : 1;
}
