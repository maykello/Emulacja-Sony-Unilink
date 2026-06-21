// ============================================================================
// Property test: parser tnie strumien dokladnie po granicach wyznaczonych
// przez CMD1 (model parsera, bez ISR).
//
// Feature: unilink-kompendium-alignment, Property 4: Dla dowolnej sekwencji sklejonych poprawnych ramek model parsera wyodrebnia te same ramki wg długości z CMD1
// Validates: Requirements 3.4, 3.5
//
// Tlo: UnilinkParserModel::parseNextFrame jest czysta funkcja (bez Arduino.h,
// bez ISR), odwzorowujaca kryterium podstawowe UnilinkBus::readFrame: granica
// ramki wyznaczana jest przez dlugosc z CMD1 (buf[2]), a NIE przez cisze. Ta
// wlasnosc sprawdza, ze dla dowolnej sekwencji poprawnych ramek sklejonych w
// jeden strumien bajtow model parsera odtwarza DOKLADNIE te same ramki (te same
// granice i te same bajty) co oryginaly (R3.4, R3.5).
//
// Uklady ramek (Kompendium §3/§4):
//   short  (6B):  RAD TAD CMD1 CMD2 P1 0           (CMD1 < 0x80)
//   middle (11B): RAD TAD CMD1 CMD2 P1 D1 D2 D3 D4 P2 0      (0x80..0xBF)
//   long   (16B): RAD TAD CMD1 CMD2 P1 D1..D9 P2 0           (>= 0xC0)
// ============================================================================
#include <cstdint>
#include <vector>

#include <rapidcheck.h>

#include "UnilinkFrame.h"
#include "UnilinkParserModel.h"

namespace {

// Generator pojedynczego bajtu z pelnego zakresu 0x00-0xFF.
rc::Gen<uint8_t> genByte() {
    return rc::gen::cast<uint8_t>(rc::gen::inRange(0, 256));
}

// Buduje kompletna, POPRAWNA ramke (z policzonymi parzystosciami i bajtem
// koncowym 0) dla zadanej liczby bajtow danych: 0 = short, 4 = middle,
// 9 = long. CMD1 dobierany przez wywolujacego tak, by byl spojny z rozmiarem.
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
        frame.push_back(0x00);  // short: brak Parity2, tylko bajt koncowy
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

// Losuje CMD1 spojny z wybranym rozmiarem ramki:
//   size 0 (short)  -> CMD1 < 0x80          (0x00..0x7F)
//   size 1 (middle) -> 0x80 <= CMD1 < 0xC0  (0x80..0xBF)
//   size 2 (long)   -> CMD1 >= 0xC0         (0xC0..0xFF)
uint8_t genCmd1ForSize(int sizeSel) {
    switch (sizeSel) {
        case 0: return *rc::gen::cast<uint8_t>(rc::gen::inRange(0x00, 0x80));
        case 1: return *rc::gen::cast<uint8_t>(rc::gen::inRange(0x80, 0xC0));
        default: return *rc::gen::cast<uint8_t>(rc::gen::inRange(0xC0, 0x100));
    }
}

// Buduje pojedyncza poprawna ramke o losowo wybranym rozmiarze (short/middle/
// long) z losowymi polami; CMD1 jest spojny z rozmiarem.
std::vector<uint8_t> genRandomFrame() {
    const int sizeSel = *rc::gen::inRange(0, 3);  // 0=short, 1=middle, 2=long
    const uint8_t rad = *genByte();
    const uint8_t tad = *genByte();
    const uint8_t cmd1 = genCmd1ForSize(sizeSel);
    const uint8_t cmd2 = *genByte();

    int dataLen = 0;
    if (sizeSel == 1) dataLen = 4;
    else if (sizeSel == 2) dataLen = 9;

    std::vector<uint8_t> data;
    for (int i = 0; i < dataLen; ++i) {
        data.push_back(*genByte());
    }
    return buildFrame(rad, tad, cmd1, cmd2, data);
}

}  // namespace

int main() {
    bool allPassed = true;

    // Property 4: dla dowolnej sekwencji sklejonych poprawnych ramek model
    // parsera wyodrebnia DOKLADNIE te same ramki, tnac strumien po granicach
    // wyznaczonych przez CMD1 (a nie po ciszy).
    allPassed &= rc::check(
        "Property 4: parser tnie strumien po granicach CMD1 (round-trip ramek)",
        []() {
            // 1) Wygeneruj losowa sekwencje poprawnych ramek (1..8 sztuk).
            const int frameCount = *rc::gen::inRange(1, 9);
            std::vector<std::vector<uint8_t>> originals;
            originals.reserve(frameCount);
            for (int i = 0; i < frameCount; ++i) {
                originals.push_back(genRandomFrame());
            }

            // 2) Sklej je w jeden strumien bajtow.
            std::vector<uint8_t> stream;
            for (const auto& f : originals) {
                stream.insert(stream.end(), f.begin(), f.end());
            }

            // 3) Przejedz modelem parsera po strumieniu: wyodrebniaj ramki
            //    wg dlugosci z CMD1, przesuwajac sie o zwrocona dlugosc.
            std::vector<std::vector<uint8_t>> parsed;
            int offset = 0;
            const int total = static_cast<int>(stream.size());
            while (offset < total) {
                uint8_t out[16] = {0};
                const int n = UnilinkParserModel::parseNextFrame(
                    stream.data() + offset, total - offset, out, sizeof(out));
                // Strumien zlozony wylacznie z poprawnych ramek MUSI dac
                // kompletna ramke na kazdej granicy (postep > 0).
                RC_ASSERT(n > 0);
                parsed.emplace_back(out, out + n);
                offset += n;
            }

            // 4) Te same ramki (te same granice i te same bajty) co oryginaly.
            RC_ASSERT(parsed.size() == originals.size());
            for (size_t i = 0; i < originals.size(); ++i) {
                RC_ASSERT(parsed[i] == originals[i]);
            }
            // Strumien skonsumowany dokladnie (brak resztek) — ciecie po CMD1.
            RC_ASSERT(offset == total);
        });

    return allPassed ? 0 : 1;
}
