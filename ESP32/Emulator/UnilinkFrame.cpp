#include "UnilinkFrame.h"

// =============================================================================
// UnilinkFrame — implementacja czystej logiki ramek UniLink.
// Brak zaleznosci sprzetowych: tylko arytmetyka na bajtach.
// =============================================================================

namespace UnilinkFrame {

// --- MAPOWANIE STANU MECHANIZMU NA BAJT STATUSU (Kompendium §7.1, Wymaganie 5) ---
// Czyste, beznosprzetowe mapowanie wewnetrznego MechState na kod statusu
// wysylany w PONG / ramkach statusu. Zwracane kody nalezza do zbioru
// {0x00, 0x20, 0x21, 0x40, 0x80, 0xC0} (Property 7 / zadanie 6.3).
uint8_t statusByte(MechState s) {
    switch (s) {
        case MechState::Playing:      return 0x00;  // §7.1 Playing
        case MechState::ChangedCd:    return 0x20;  // §7.1 Changed CD
        case MechState::Seeking:      return 0x21;  // §7.1 Seeking (FF/REW)
        case MechState::Changing:     return 0x40;  // §7.1 Changing CD
        case MechState::LoadingTrack: return 0x40;  // §7.1 Changing CD (ladowanie utworu)
        case MechState::Idle:         return 0x80;  // §7.1 Idle
        case MechState::Ejecting:     return 0xC0;  // §7.1 Ejecting
        // [DEVIATION §7.1] Init nie ma wlasnego kodu w §7.1. Dawniej STATE_INIT
        // = 0xC0 (Ejecting), co bylo semantycznie bledne. Mapujemy Init -> 0x80
        // (Idle): mechanizm gotowy, jeszcze nie gra — zgodne z protokolem. Patrz
        // design.md §5 (mozna swiadomie przywrocic 0xC0, gdyby radio tego
        // wymagalo w fazie inicjalizacji).
        case MechState::Init:         return 0x80;  // [DEVIATION §7.1]
    }
    return 0x80;  // bezpieczny domyslny (Idle)
}

// --- DLUGOSC RAMKI WG CMD1 (Kompendium §3) ---
FrameSize sizeFromCmd1(uint8_t cmd1) {
    if (cmd1 < 0x80) {
        return FrameSize::Short;   // 6
    }
    if (cmd1 < 0xC0) {             // 0x80..0xBF
        return FrameSize::Middle;  // 11
    }
    return FrameSize::Long;        // 0xC0..0xFF -> 16
}

int lengthFromCmd1(uint8_t cmd1) {
    return static_cast<int>(sizeFromCmd1(cmd1));
}

// --- SUMY KONTROLNE (Kompendium §4) ---
uint8_t parity1(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2) {
    // (RAD + TAD + CMD1 + CMD2) mod 256 — przepelnienie do 8 bitow daje mod 256.
    return static_cast<uint8_t>(rad + tad + cmd1 + cmd2);
}

uint8_t parity2(uint8_t parity1, const uint8_t* data, int dataLen) {
    uint8_t sum = parity1;
    if (data != nullptr) {
        for (int i = 0; i < dataLen; ++i) {
            sum = static_cast<uint8_t>(sum + data[i]);
        }
    }
    return sum;   // (Parity1 + suma bajtow danych) mod 256
}

// --- WALIDACJA KOMPLETNEJ RAMKI (Wymaganie 2) ---
ValidateResult validate(const uint8_t* frame, int len) {
    if (frame == nullptr) {
        return ValidateResult::BadLength;
    }
    // Rozpoznaj strukture po dlugosci przekazanej przez warstwe odbioru.
    if (len != 6 && len != 11 && len != 16) {
        return ValidateResult::BadLength;
    }

    // Parity1 sprawdzane zawsze (short/middle/long) — R2.2, R2.4.
    const uint8_t p1 = parity1(frame[0], frame[1], frame[2], frame[3]);
    if (frame[4] != p1) {
        return ValidateResult::BadParity1;
    }

    if (len == 6) {
        // Short: brak Parity2; sprawdz tylko bajt koncowy (R2.4).
        if (frame[5] != 0x00) {
            return ValidateResult::BadEnd;
        }
        return ValidateResult::Ok;
    }

    // Middle (11) / Long (16): dane od indeksu 5, Parity2 przedostatni bajt.
    const int dataLen = len - 7;            // middle: 4, long: 9
    const uint8_t p2 = parity2(p1, &frame[5], dataLen);
    if (frame[len - 2] != p2) {             // R2.1, R2.3
        return ValidateResult::BadParity2;
    }
    if (frame[len - 1] != 0x00) {
        return ValidateResult::BadEnd;
    }
    return ValidateResult::Ok;
}

// --- KODOWANIE NUMERU PLYTY F|nr (Wymaganie 7.4) ---
uint8_t encodeDiscNibble(uint8_t discNumber) {
    // Gorny nibble F, numer w dolnym nibblu: 1 -> 0xF1 ... 9 -> 0xF9.
    return static_cast<uint8_t>(0xF0 | (discNumber & 0x0F));
}

uint8_t discNibbleToNumber(uint8_t encoded) {
    return static_cast<uint8_t>(encoded & 0x0F);   // 0xF1 -> 1
}

// --- KODOWANIE BCD CZASU/UTWORU (Wymaganie 11) ---
uint8_t encodeBcd(uint8_t value) {
    // 59 -> 0x59, 7 -> 0x07.
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t decodeBcd(uint8_t bcd) {
    // 0x59 -> 59.
    return static_cast<uint8_t>(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

uint8_t encodeBcdFpad(uint8_t value) {
    // Wartosci < 10 padowane gornym nibblem F (7 -> 0xF7); >= 10 klasyczny BCD
    // (12 -> 0x12).
    if (value < 10) {
        return static_cast<uint8_t>(0xF0 | value);
    }
    return encodeBcd(value);
}

// --- KODOWANIE IKON TRYBOW Repeat/Shuffle/Intro (ramka 0x94, Wymaganie 8.4) ---
// Uklad bitow opisany w UnilinkFrame.h. encode/decode sa wzajemnie odwrotne dla
// repeatMode w {0,1,2}; decode maskuje repeat do 2 bitow (0..3), a shuffle/intro
// do pojedynczych bitow, wiec round-trip dla poprawnego PlayModes jest pelny.
void encodeIconData(uint8_t repeatMode, bool shuffle, bool intro, uint8_t* out) {
    if (out == nullptr) {
        return;
    }
    uint8_t d1 = 0;
    if (shuffle) d1 |= 0x01;                              // bit0 = shuffle
    if (intro)   d1 |= 0x02;                              // bit1 = intro
    d1 |= static_cast<uint8_t>((repeatMode & 0x03) << 4); // bity 4-5 = repeat
    out[0] = d1;
    out[1] = 0x00;   // D2 rezerwa
    out[2] = 0x00;   // D3 rezerwa
    out[3] = 0x00;   // D4 rezerwa
}

void decodeIconData(const uint8_t* data, uint8_t& repeatMode, bool& shuffle, bool& intro) {
    if (data == nullptr) {
        repeatMode = 0;
        shuffle = false;
        intro = false;
        return;
    }
    const uint8_t d1 = data[0];
    shuffle    = (d1 & 0x01) != 0;
    intro      = (d1 & 0x02) != 0;
    repeatMode = static_cast<uint8_t>((d1 >> 4) & 0x03);
}

} // namespace UnilinkFrame
