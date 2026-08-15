// =============================================================================
// Magazine.cpp — czesc sprzetowa modulu magazynka (Wymaganie 7)
// =============================================================================
// Tutaj zyje logika powiazana ze sprzetem/nosnikiem: odpytywanie AudioPlayer o
// liczbe utworow na plytach oraz cache deterministycznych identyfikatorow plyt.
// Czysta matematyka (mapa obecnosci, generowanie ID, kodowanie F|nr) jest w
// Magazine.h jako funkcje inline — testowalne natywnie bez Arduino.
// =============================================================================

#include "Magazine.h"
#include "AudioPlayer.h"
#include "Config.h"

namespace Magazine {

// --- CACHE IDENTYFIKATOROW PLYT (stale miedzy zadaniami, Wymaganie 7.5) ---
// Disc ID liczony jest deterministycznie z numeru plyty, ale cache'ujemy go per
// plyta, by uniknac ponownego liczenia i miec jedno zrodlo prawdy dla 0xC5/0xD5.
static uint32_t s_discIdCache[MAX_DISC]  = {0};
static bool     s_discIdCached[MAX_DISC] = {false};

// Zwroc (i w razie potrzeby wylicz) zcache'owany identyfikator plyty.
static uint32_t cachedDiscId(uint8_t disc) {
    if (disc < 1 || disc > MAX_DISC) {
        return computeDiscId(disc);   // poza zakresem — bez cache, ale deterministycznie
    }
    uint8_t idx = static_cast<uint8_t>(disc - 1);
    if (!s_discIdCached[idx]) {
        s_discIdCache[idx]  = computeDiscId(disc);
        s_discIdCached[idx] = true;
    }
    return s_discIdCache[idx];
}

uint16_t presenceMap() {
    // Obecnosc plyty wynika z liczby utworow w jej folderze (CD01..CD10).
    uint8_t counts[MAX_DISC];
    for (uint8_t i = 0; i < MAX_DISC; ++i) {
        counts[i] = audioGetTrackCount(static_cast<uint8_t>(i + 1));
    }
    return presenceMapFrom(counts, MAX_DISC);
}

void buildDiscInfo(uint8_t disc, uint8_t* d) {
    // Liczba utworow z nosnika. AudioPlayer nie udostepnia calkowitego czasu
    // plyty, wiec bierzemy go z tego samego deterministycznego "TOC-a", ktory
    // zasila ramke 0xC5/0xD5 — dzieki temu 0x97 i 0xD5 raportuja SPOJNY czas
    // plyty (radio potrafi je porownywac przy dopasowaniu Custom File).
    uint8_t trackCount = audioGetTrackCount(disc);
    uint8_t minutes = 0, seconds = 0, hundredths = 0;
    discIdToc(cachedDiscId(disc), minutes, seconds, hundredths);
    if (trackCount == 0) {
        // Pusta szuflada: prawdziwa zmieniarka raportuje 99 utworow i czas 00:00.
        buildDiscInfoData(disc, 99, 0, 0, d);
        return;
    }
    buildDiscInfoData(disc, trackCount, minutes, seconds, d);
}

void buildDiscId(uint8_t disc, bool discChangeVariant, uint8_t& cmd2, uint8_t* d) {
    // Stały, cache'owany identyfikator -> ten sam disc daje te same bajty
    // miedzy kolejnymi zadaniami (Wymaganie 7.5).
    uint8_t trackCount = audioGetTrackCount(disc);
    if (trackCount == 0) trackCount = 99;
    fillDiscIdData(disc, cachedDiscId(disc), trackCount, discChangeVariant, cmd2, d);
}

} // namespace Magazine
