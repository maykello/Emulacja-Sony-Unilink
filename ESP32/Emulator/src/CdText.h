#ifndef CD_TEXT_H
#define CD_TEXT_H

#include <stdint.h>
#include <stddef.h>

// =============================================================================
// CdText (Modul_CD_TEXT) — czysta logika podzialu nazw na pola CD-TEXT
// =============================================================================
// Modul CELOWO nie zalezy od Arduino.h, ISR ani globalnego stanu sprzetu —
// tylko od <stdint.h>/<stddef.h>. Dzieki temu kompiluje sie zarowno na ESP32
// (w szkicu Arduino), jak i natywnie na hoscie (g++/clang) na potrzeby testow
// property-based (Property 8/9/10).
//
// Funkcje operuja na PRZEKAZANEJ nazwie `name` (string zakonczony NUL-em).
// Dyspozytor (UnilinkProtocol::handlePacket) pobiera nazwe z AudioPlayer
// (audioGetTrackName / audioGetDiscName), sanityzuje ja przez sanitizeAscii,
// a nastepnie dzieli na pola przez buildField8 / buildFieldD2.
//
// Uklad ramek nazw (Kompendium §10):
//   - Wariant 8-znakowy (§10.2): pola po 8 znakow, offset = field*8.
//       pola 0–1 -> CMD1 0xC9 (utwor) / 0xCD (plyta)
//       pola 2–5 -> CMD1 0xD9 (utwor) / 0xDD (plyta)
//   - Wariant 0xD2 (§10.3, tryb CD): 6 znakow na ramke, PIERWSZY znak w CMD2,
//       offset = field*6.
// =============================================================================

namespace CdText {

// --- STALE PROTOKOLU ---
// Liczba znakow na pole dla obu wariantow.
constexpr int FIELD8_CHARS = 8;   // wariant klasyczny 0xC9/0xD9 (§10.2)
constexpr int FIELDD2_CHARS = 6;  // wariant 0xD2 w trybie CD (§10.3)
// Najwyzszy dopuszczalny numer pola (0..5 => do 6 pol).
constexpr int MAX_FIELD = 5;

// Komendy CD-TEXT (CMD1) wg numeru pola.
constexpr uint8_t CMD_TRACK_LOW  = 0xC9;  // nazwa utworu, pola 0–1
constexpr uint8_t CMD_TRACK_HIGH = 0xD9;  // nazwa utworu, pola 2–5
constexpr uint8_t CMD_DISC_LOW   = 0xCD;  // nazwa plyty,  pola 0–1
constexpr uint8_t CMD_DISC_HIGH  = 0xDD;  // nazwa plyty,  pola 2–5

// --- SANITYZACJA (R6.6, Property 10) ---
// Kopiuje z `in` do `out` wylacznie bajty z drukowalnego ASCII (0x20..0x7E);
// pozostale bajty sa pomijane. Wynik jest zawsze zakonczony NUL-em i miesci sie
// w `maxLen` (lacznie z terminatorem). Zwraca liczbe zapisanych znakow (bez
// terminatora). Dla in==NULL / out==NULL / maxLen==0 zwraca 0.
size_t sanitizeAscii(const char* in, char* out, size_t maxLen);

// --- WARIANT 8-ZNAKOWY (R6.3/R6.4, §10.2) ---
// Wypisuje do `outChars` (bufor [8]) znaki nazwy `name` od offsetu field*8,
// maksymalnie 8 znakow, zatrzymujac sie na koncu napisu. Bajty spoza
// drukowalnego ASCII sa zastepowane spacja (0x20), wiec WSZYSTKIE wypisane
// bajty naleza do 0x20..0x7E (Property 10). Pozostala czesc bufora (gdy znakow
// jest < 8) wypelniana jest spacja. Zwraca liczbe rzeczywistych znakow nazwy
// w polu (0..8).
int buildField8(const char* name, int field, uint8_t* outChars /*[8]*/);

// --- WARIANT 0xD2 (R6.7, §10.3) ---
// Jak buildField8, ale pole ma 6 znakow, a offset = field*6. Na poziomie ramki
// pierwszy z tych 6 znakow trafia do CMD2 — ta funkcja zwraca po prostu 6
// znakow pola; rozmieszczenie w ramce robi dyspozytor. Zwraca liczbe
// rzeczywistych znakow (0..6).
int buildFieldD2(const char* name, int field, uint8_t* outChars /*[6]*/);

// --- ISTNIENIE POLA (R6.5) ---
// true wtedy i tylko wtedy, gdy field jest w zakresie [0, MAX_FIELD] ORAZ offset
// (field*charsPerField) jest mniejszy od dlugosci nazwy (tekst sie nie wyczerpal).
bool fieldExists(const char* name, int field, int charsPerField);

// --- ZLOZENIE NAZWY Z KOLEJNYCH POL (R6.8, Property 8 round-trip) ---
// Sklada kolejne pola (wariant 8-znakowy gdy charsPerField==8, wariant 0xD2 gdy
// charsPerField==6) z powrotem w nazwe, odtwarzajac (sanityzowana) nazwe `name`.
// Wynik zakonczony NUL-em, ograniczony do maxLen (lacznie z terminatorem).
// Zwraca liczbe zapisanych znakow (bez terminatora).
size_t reassemble(const char* name, int charsPerField, char* out, size_t maxLen);

// --- WYBOR KOMENDY WG NUMERU POLA (R6.3/R6.4, Property 9) ---
// Pola 0–1 -> 0xC9 (utwor) / 0xCD (plyta); pola 2–5 -> 0xD9 (utwor) / 0xDD
// (plyta). Dla isDisc==true zwraca warianty plyty, w przeciwnym razie utworu.
uint8_t commandForField(int field, bool isDisc);

} // namespace CdText

#endif // CD_TEXT_H
