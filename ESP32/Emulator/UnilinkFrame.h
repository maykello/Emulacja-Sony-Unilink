#ifndef UNILINK_FRAME_H
#define UNILINK_FRAME_H

#include <stdint.h>
#include <stddef.h>

// =============================================================================
// UnilinkFrame — czysta logika "matematyki ramek" Sony UniLink
// =============================================================================
// Modul CELOWO nie zalezy od Arduino.h, ISR ani globalnego stanu sprzetu —
// tylko od <stdint.h>/<stddef.h>. Dzieki temu kompiluje sie zarowno na ESP32
// (w szkicu Arduino), jak i natywnie na hoscie (g++/clang) na potrzeby testow
// property-based. Cala wiedza o dlugosci ramki (wg CMD1), sumach kontrolnych,
// kodowaniu numeru plyty (F|nr) i BCD czasu zyje tutaj, jako jedno zrodlo prawdy
// dla nadawania i walidacji.
//
// Uklad ramek UniLink (z bajtem koncowym END=0x00):
//   short  (6B):  RAD TAD CMD1 CMD2 Parity1 0
//   middle (11B): RAD TAD CMD1 CMD2 Parity1 D1 D2 D3 D4 Parity2 0
//   long   (16B): RAD TAD CMD1 CMD2 Parity1 D1 D2 D3 D4 D5 D6 D7 D8 D9 Parity2 0
//
//   Parity1 = (RAD + TAD + CMD1 + CMD2) mod 256
//   Parity2 = (Parity1 + suma bajtow danych) mod 256
// =============================================================================

namespace UnilinkFrame {

// --- WEWNETRZNY STAN MECHANIZMU ZMIENIARKI (Kompendium §7.1, Wymaganie 5) ---
// Definicja zyje w czystym module UnilinkFrame (bez Arduino.h), aby mapowanie
// stanu na bajt statusu statusByte() bylo w pelni czyste i testowalne na hoscie
// (Property 7 / zadanie 6.3). CdChanger uzywa tego typu przez alias
// `CdChanger::MechState = UnilinkFrame::MechState`.
enum class MechState : uint8_t {
    Init,         // budzenie po sesji
    Idle,         // gotowy, nie gra
    Changing,     // trwa zmiana plyty
    ChangedCd,    // plyta zmieniona (krotki stan przejsciowy)
    LoadingTrack, // ladowanie/szukanie utworu na plycie
    Playing,      // odtwarzanie
    Seeking,      // przewijanie FF/REW
    Ejecting,     // wysuwanie
};

// --- MAPOWANIE STANU NA BAJT STATUSU §7.1 (Wymaganie 5) ---
// Zwraca kod statusu wysylany w PONG / ramkach statusu:
//   Playing -> 0x00, ChangedCd -> 0x20, Seeking -> 0x21,
//   Changing/LoadingTrack -> 0x40, Idle -> 0x80, Ejecting -> 0xC0,
//   Init -> 0x80 ([DEVIATION §7.1] — patrz design.md §5).
uint8_t statusByte(MechState s);

// Dlugosc ramki wyrazona w bajtach (wraz z bajtem koncowym).
enum class FrameSize : uint8_t { Short = 6, Middle = 11, Long = 16 };

// --- DLUGOSC RAMKI WG CMD1 (Kompendium §3, Wymaganie 3) ---
//  CMD1 < 0x80           -> Short  (6)
//  0x80 <= CMD1 < 0xC0   -> Middle (11)
//  CMD1 >= 0xC0          -> Long   (16)
FrameSize sizeFromCmd1(uint8_t cmd1);
int       lengthFromCmd1(uint8_t cmd1);   // 6 / 11 / 16

// --- SUMY KONTROLNE (Kompendium §4, Wymaganie 2) ---
// Parity1 = (RAD + TAD + CMD1 + CMD2) mod 256.
uint8_t parity1(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2);
// Parity2 = (Parity1 + suma bajtow danych) mod 256.
uint8_t parity2(uint8_t parity1, const uint8_t* data, int dataLen);

// --- WALIDACJA KOMPLETNEJ RAMKI (Wymaganie 2) ---
// Short  -> sprawdza tylko Parity1 (Parity2 nie wystepuje) oraz bajt koncowy 0.
// Middle/Long -> sprawdza Parity1 ORAZ Parity2 oraz bajt koncowy 0.
enum class ValidateResult : uint8_t { Ok, BadLength, BadParity1, BadParity2, BadEnd };
ValidateResult validate(const uint8_t* frame, int len);

// --- KODOWANIE NUMERU PLYTY F|nr (Wymaganie 7.4) ---
uint8_t encodeDiscNibble(uint8_t discNumber);   // 1 -> 0xF1, 4 -> 0xF4
uint8_t discNibbleToNumber(uint8_t encoded);     // 0xF1 -> 1 (odwrotnosc)

// --- KODOWANIE BCD CZASU/UTWORU (Wymaganie 11) ---
uint8_t encodeBcd(uint8_t value);                // 59 -> 0x59, 7 -> 0x07
uint8_t decodeBcd(uint8_t bcd);                  // 0x59 -> 59
uint8_t encodeBcdFpad(uint8_t value);            // 7 -> 0xF7, 12 -> 0x12

// --- KODOWANIE IKON TRYBOW Repeat/Shuffle/Intro (ramka 0x94, Wymaganie 8.4) ---
// Czysta, beznosprzetowa para encode/decode stanu trybow <-> bajty danych ramki
// ikon `0x94`. Trzymana TUTAJ (a nie w CdChanger), aby round-trip stan<->ramka
// (Property 15 / zadanie 11.5) byl w pelni testowalny na hoscie, bez Arduino.
// CdChanger::PlayModes nie jest tu widoczny (zalezy od Arduino.h), wiec operujemy
// na typach prymitywnych: `repeatMode` = 0 (Off) / 1 (One) / 2 (All).
//
// 0x94 to CMD1 w zakresie 0x80..0xBF => ramka middle (4 bajty danych D1..D4).
// UKLAD (samospojny, round-trippable dla repeatMode w {0,1,2}):
//   D1 bit0      = shuffle
//   D1 bit1      = intro
//   D1 bity 4-5  = repeat mode (0/1/2)  (gorny nibble, dolne 2 bity)
//   D2 = D3 = D4 = 0x00 (rezerwa)
constexpr int ICON_DATA_LEN = 4;

// Zakoduj stan trybow do `out[ICON_DATA_LEN]` (D1..D4 ramki 0x94).
void encodeIconData(uint8_t repeatMode, bool shuffle, bool intro, uint8_t* out);

// Zdekoduj bajty danych ramki 0x94 z powrotem do stanu trybow. Odwrotnosc
// encodeIconData dla repeatMode w {0,1,2} (Property 15).
void decodeIconData(const uint8_t* data, uint8_t& repeatMode, bool& shuffle, bool& intro);

} // namespace UnilinkFrame

#endif // UNILINK_FRAME_H
