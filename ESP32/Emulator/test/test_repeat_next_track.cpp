// ============================================================================
// Property test: wybor nastepnego utworu wg Repeat (CdChanger::serviceAutoAdvance).
//
// Feature: unilink-kompendium-alignment, Property 16: One zwraca ten sam utwór, All zawija kolejność, Off zatrzymuje po ostatnim utworze ostatniej płyty
// Validates: Requirements 8.5
//
// Tlo: Funkcja serviceAutoAdvance w CdChanger wybiera nastepny utwor w
// zaleznosci od trybu Repeat:
//   - RepeatMode::One: zawsze zwraca ten sam utwor (powtarza).
//   - RepeatMode::All: po ostatnim utworze ostatniej płyty zawija do 1/1.
//   - RepeatMode::Off: po ostatnim utworze ostatniej płyty zostaje na ostatnim
//     utworze (nie awansuje).
//
// Ta testowana jest czysta funkcja modelNextTrack, ktora wydziela logike
// wyboru nastepnego utworu. Funkcja przyjmuje:
//   - modes: biezace tryby (szczególnie repeat)
//   - disc, track: biezaca pozycja (1..maxDisc, 1..maxTrack)
//   - maxDisc, maxTrack: maksymalne numery (zwykle MAX_DISC=10, MAX_TRACK=99)
// Zwraca (nextDisc, nextTrack) zgodnie z trybem Repeat.
// ============================================================================
#include <cstdint>
#include <tuple>

#include <rapidcheck.h>

#include "CdChanger.h"

// Wyciagniety prototyp czystej funkcji (implementacja w CdChanger.cpp).
namespace CdChanger {

// Dla testów host-side potrzebujemy wyjsciowej funkcji modelNextTrack.
// Poniewaz serviceAutoAdvance jest inline lub implementacja jest w .cpp,
// dodajemy deklaracje tutaj, a implementacje dodajemy w CdChanger.cpp.
struct PlayModes modelNextTrack(PlayModes modes, uint8_t disc, uint8_t track,
                                uint8_t maxDisc, uint8_t maxTrack);

} // namespace CdChanger

int main() {
    bool allPassed = true;

    // Property 16: Tryb Repeat steruje wyborem nastepnego utworu.
    allPassed &= rc::check(
        "Property 16: Tryb Repeat steruje wyborem nastepnego utworu",
        []() {
            // Generuj dowolne tryby odtwarzania.
            const auto repeatMode =
                static_cast<RepeatMode>(*rc::gen::inRange(
                    static_cast<uint8_t>(RepeatMode::Off),
                    static_cast<uint8_t>(RepeatMode::All) + 1));
            const bool shuffle = *rc::gen::operator!(rc::gen::any<bool>());
            const bool intro   = *rc::gen::operator!(rc::gen::any<bool>());
            const PlayModes modes = { repeatMode, shuffle, intro };

            // Generuj dowolna plyte i utwor w poprawnym zakresie (1..max).
            const uint8_t maxDisc  = 10;  // MAX_DISC
            const uint8_t maxTrack = 99;  // MAX_TRACK_PER_DISC
            const uint8_t disc =
                static_cast<uint8_t>(*rc::gen::inRange(1, maxDisc + 1));
            const uint8_t track =
                static_cast<uint8_t>(*rc::gen::inRange(1, maxTrack + 1));

            const uint8_t nextDisc = CdChanger::modelNextTrack(
                modes, disc, track, maxDisc, maxTrack).nextDisc;
            const uint8_t nextTrack = CdChanger::modelNextTrack(
                modes, disc, track, maxDisc, maxTrack).nextTrack;

            // --------------------------------------------------------------
            // Property 16a: RepeatMode::One — zawsze ten sam utwor.
            // --------------------------------------------------------------
            if (repeatMode == RepeatMode::One) {
                RC_ASSERT(nextDisc == disc);
                RC_ASSERT(nextTrack == track);
            }

            // --------------------------------------------------------------
            // Property 16b: RepeatMode::All — cykliczna kolejnosc.
            // Po ostatnim utworze ostatniej plyty -> 1/1.
            // --------------------------------------------------------------
            if (repeatMode == RepeatMode::All) {
                const bool isLastDisc  = (disc == maxDisc);
                const bool isLastTrack = (track == maxTrack);

                if (isLastDisc && isLastTrack) {
                    // Last disc + last track -> wrap to 1/1.
                    RC_ASSERT(nextDisc == 1);
                    RC_ASSERT(nextTrack == 1);
                } else if (isLastTrack) {
                    // Last track of non-last disc -> next disc, track 1.
                    RC_ASSERT(nextDisc == disc + 1);
                    RC_ASSERT(nextTrack == 1);
                } else {
                    // Normal advance: same disc, next track.
                    RC_ASSERT(nextDisc == disc);
                    RC_ASSERT(nextTrack == track + 1);
                }
            }

            // --------------------------------------------------------------
            // Property 16c: RepeatMode::Off — brak awansu po koncu.
            // Po ostatnim utworze ostatniej plyty zostaje na ostatnim.
            // --------------------------------------------------------------
            if (repeatMode == RepeatMode::Off) {
                const bool isLastDisc  = (disc == maxDisc);
                const bool isLastTrack = (track == maxTrack);

                if (isLastDisc && isLastTrack) {
                    // Last disc + last track -> stay on last track.
                    RC_ASSERT(nextDisc == disc);
                    RC_ASSERT(nextTrack == track);
                } else if (isLastTrack) {
                    // Last track of non-last disc -> next disc, track 1.
                    RC_ASSERT(nextDisc == disc + 1);
                    RC_ASSERT(nextTrack == 1);
                } else {
                    // Normal advance: same disc, next track.
                    RC_ASSERT(nextDisc == disc);
                    RC_ASSERT(nextTrack == track + 1);
                }
            }
        });

    return allPassed ? 0 : 1;
}
