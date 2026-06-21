// ============================================================================
// Unit test: zgodnosc nadawanych ramek z walidatorem UnilinkFrame.
//
// Spec: unilink-kompendium-alignment, task 2.2
// Validates: Requirements 2.1, 2.4
//
// Tlo: faktyczne funkcje nadawcze UnilinkBus::sendShort/sendMedium/sendLong
// zyja w UnilinkBus.cpp, ktory zalezy od Arduino.h i sprzetu (ISR, piny), wiec
// nie kompiluje sie natywnie na hoscie i nie da sie go wywolac w tym tescie.
// Zamiast tego replikujemy DOKLADNIE uklad/parzystosci, jakie te funkcje
// buduja (po refaktorze z task 2.1 licza sumy przez UnilinkFrame::parity1/
// parity2 i wstawiaja bajt koncowy 0x00), w lokalnych helperach i sprawdzamy
// konkretne przykladowe ramki short/middle/long.
//
// W odroznieniu od property-testow (task 1.4), to jest test JEDNOSTKOWY:
// budujemy konkretne reprezentatywne ramki kazdego rozmiaru i asercja brzmi
// UnilinkFrame::validate(frame, len) == ValidateResult::Ok.
//
// Uklady ramek odwzorowane 1:1 z UnilinkBus.cpp (z bajtem koncowym END=0x00):
//   sendShort  -> { rad, tad, cmd1, cmd2, P1, 0x00 }
//   sendMedium -> { rad, tad, cmd1, cmd2, P1, d1, d2, d3, d4, P2, 0x00 }
//   sendLong   -> { rad, tad, cmd1, cmd2, P1, d1..d9, P2, 0x00 }
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <vector>

#include "UnilinkFrame.h"

namespace {

// Replika UnilinkBus::sendShort — uklad bajtow i obliczenie Parity1.
std::vector<uint8_t> buildShort(uint8_t rad, uint8_t tad, uint8_t cmd1,
                                uint8_t cmd2) {
    const uint8_t p1 = UnilinkFrame::parity1(rad, tad, cmd1, cmd2);
    return {rad, tad, cmd1, cmd2, p1, 0x00};
}

// Replika UnilinkBus::sendMedium — Parity1 + Parity2 nad 4 bajtami danych.
std::vector<uint8_t> buildMedium(uint8_t rad, uint8_t tad, uint8_t cmd1,
                                 uint8_t cmd2, uint8_t d1, uint8_t d2,
                                 uint8_t d3, uint8_t d4) {
    const uint8_t p1 = UnilinkFrame::parity1(rad, tad, cmd1, cmd2);
    const uint8_t data[4] = {d1, d2, d3, d4};
    const uint8_t p2 = UnilinkFrame::parity2(p1, data, 4);
    return {rad, tad, cmd1, cmd2, p1, d1, d2, d3, d4, p2, 0x00};
}

// Replika UnilinkBus::sendLong — Parity1 + Parity2 nad 9 bajtami danych.
std::vector<uint8_t> buildLong(uint8_t rad, uint8_t tad, uint8_t cmd1,
                               uint8_t cmd2, uint8_t d1, uint8_t d2, uint8_t d3,
                               uint8_t d4, uint8_t d5, uint8_t d6, uint8_t d7,
                               uint8_t d8, uint8_t d9) {
    const uint8_t p1 = UnilinkFrame::parity1(rad, tad, cmd1, cmd2);
    const uint8_t data[9] = {d1, d2, d3, d4, d5, d6, d7, d8, d9};
    const uint8_t p2 = UnilinkFrame::parity2(p1, data, 9);
    return {rad, tad, cmd1, cmd2, p1, d1, d2, d3, d4, d5, d6, d7, d8, d9, p2,
            0x00};
}

// Pojedynczy przypadek: ramka o oczekiwanej dlugosci przechodzi validate==Ok.
bool expectOk(const char* label, const std::vector<uint8_t>& frame,
              size_t expectedLen) {
    bool ok = true;
    if (frame.size() != expectedLen) {
        std::printf("FAIL [%s]: dlugosc %zu != oczekiwana %zu\n", label,
                    frame.size(), expectedLen);
        ok = false;
    }
    const UnilinkFrame::ValidateResult vr =
        UnilinkFrame::validate(frame.data(), static_cast<int>(frame.size()));
    if (vr != UnilinkFrame::ValidateResult::Ok) {
        std::printf("FAIL [%s]: validate zwrocil %d (oczekiwano Ok=0)\n", label,
                    static_cast<int>(vr));
        ok = false;
    }
    if (ok) std::printf("PASS [%s]\n", label);
    return ok;
}

}  // namespace

int main() {
    bool allPassed = true;

    // --- Short (6B): PONG/ACK na adres radia ---
    // CMD1 < 0x80 => short. Reprezentatywne wartosci sesji UniLink.
    allPassed &= expectOk("short: PONG 0x10<-0x31 cmd 0x70/0x00",
                          buildShort(0x10, 0x31, 0x70, 0x00), 6);
    allPassed &= expectOk("short: ACK 0x18<-0x30 cmd 0x01/0x12",
                          buildShort(0x18, 0x30, 0x01, 0x12), 6);
    // Skrajne wartosci pol naglowka (przepelnienie sumy mod 256).
    allPassed &= expectOk("short: skrajne 0xFF/0xFF/0x7F/0xFF",
                          buildShort(0xFF, 0xFF, 0x7F, 0xFF), 6);

    // --- Middle (11B): 0x80 <= CMD1 < 0xC0 ---
    // np. ramka ikon trybow 0x94, info magazynka 0x95/0x97.
    allPassed &= expectOk("middle: ikony 0x94 (repeat/shuffle/intro)",
                          buildMedium(0x70, 0x31, 0x94, 0x00, 0x01, 0x00, 0x00,
                                      0x00),
                          11);
    allPassed &= expectOk("middle: mapa magazynka 0x95",
                          buildMedium(0x10, 0x31, 0x95, 0x00, 0xFF, 0x03, 0x00,
                                      0x00),
                          11);
    // Skrajne wartosci naglowka + danych.
    allPassed &= expectOk("middle: skrajne pola 0xFF...",
                          buildMedium(0xFF, 0xFF, 0xBF, 0xFF, 0xFF, 0xFF, 0xFF,
                                      0xFF),
                          11);

    // --- Long (16B): CMD1 >= 0xC0 ---
    // np. pelny status 0xC0 (plyta/utwory/min/sek), disc ID 0xC5/0xD5.
    allPassed &= expectOk("long: pelny status 0xC0",
                          buildLong(0x10, 0x31, 0xC0, 0x00, 0xF1, 0x12, 0x00,
                                    0x34, 0x56, 0x00, 0x00, 0x00, 0x00),
                          16);
    allPassed &= expectOk("long: disc ID 0xD5",
                          buildLong(0x70, 0x31, 0xD5, 0x00, 0xF2, 0x11, 0x22,
                                    0x33, 0x44, 0x55, 0x66, 0x77, 0x88),
                          16);
    // Skrajne wartosci naglowka + danych.
    allPassed &= expectOk("long: skrajne pola 0xFF...",
                          buildLong(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF),
                          16);

    if (allPassed) {
        std::printf("ALL PASSED: nadawane ramki short/middle/long -> validate Ok\n");
    } else {
        std::printf("SOME TESTS FAILED\n");
    }
    return allPassed ? 0 : 1;
}
