#include "CdText.h"

// =============================================================================
// CdText — implementacja czystej logiki podzialu nazw na pola CD-TEXT.
// Brak zaleznosci od Arduino.h: tylko proste operacje na bajtach. Wlasna
// implementacja dlugosci napisu, by nie wymagac nawet <string.h>.
// =============================================================================

namespace CdText {

namespace {

// Dlugosc napisu zakonczonego NUL-em (NULL -> 0).
size_t cstrLen(const char* s) {
    if (s == nullptr) {
        return 0;
    }
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

// Czy bajt nalezy do drukowalnego ASCII (0x20..0x7E)?
inline bool isPrintableAscii(unsigned char c) {
    return c >= 0x20 && c <= 0x7E;
}

// Wspolna budowa pola o stalej szerokosci `width` i offsecie field*width.
// Wypisuje do outChars[width]: znaki nazwy (spoza ASCII -> spacja), reszta
// bufora dopelniona spacja. Zwraca liczbe rzeczywistych znakow nazwy (0..width).
int buildFieldWidth(const char* name, int field, int width, uint8_t* outChars) {
    if (outChars == nullptr || width <= 0) {
        return 0;
    }
    // Domyslnie wypelnij pole spacja (gwarancja 0x20..0x7E na kazdej pozycji).
    for (int i = 0; i < width; ++i) {
        outChars[i] = 0x20;
    }
    if (name == nullptr || field < 0) {
        return 0;
    }
    const size_t len = cstrLen(name);
    const size_t offset = static_cast<size_t>(field) * static_cast<size_t>(width);
    if (offset >= len) {
        return 0;  // tekst sie wyczerpal — pole puste (same spacje)
    }
    int count = 0;
    for (int i = 0; i < width; ++i) {
        const size_t idx = offset + static_cast<size_t>(i);
        if (idx >= len) {
            break;  // koniec napisu — nie wypelniamy dalej znakami nazwy
        }
        unsigned char c = static_cast<unsigned char>(name[idx]);
        outChars[i] = isPrintableAscii(c) ? c : 0x20;  // Property 10
        ++count;
    }
    return count;
}

} // namespace

size_t sanitizeAscii(const char* in, char* out, size_t maxLen) {
    if (out == nullptr || maxLen == 0) {
        return 0;
    }
    size_t written = 0;
    if (in != nullptr) {
        // Zostaw miejsce na terminator (maxLen liczy terminator).
        for (size_t i = 0; in[i] != '\0' && written + 1 < maxLen; ++i) {
            unsigned char c = static_cast<unsigned char>(in[i]);
            if (isPrintableAscii(c)) {
                out[written++] = static_cast<char>(c);
            }
        }
    }
    out[written] = '\0';
    return written;
}

int buildField8(const char* name, int field, uint8_t* outChars /*[8]*/) {
    return buildFieldWidth(name, field, FIELD8_CHARS, outChars);
}

int buildFieldD2(const char* name, int field, uint8_t* outChars /*[6]*/) {
    return buildFieldWidth(name, field, FIELDD2_CHARS, outChars);
}

bool fieldExists(const char* name, int field, int charsPerField) {
    if (field < 0 || field > MAX_FIELD || charsPerField <= 0) {
        return false;
    }
    const size_t len = cstrLen(name);
    const size_t offset = static_cast<size_t>(field) * static_cast<size_t>(charsPerField);
    return offset < len;
}

size_t reassemble(const char* name, int charsPerField, char* out, size_t maxLen) {
    if (out == nullptr || maxLen == 0) {
        return 0;
    }
    size_t written = 0;
    if (name != nullptr && charsPerField > 0) {
        uint8_t field[FIELD8_CHARS];  // wystarcza dla obu wariantow (8 >= 6)
        for (int f = 0; f <= MAX_FIELD; ++f) {
            if (!fieldExists(name, f, charsPerField)) {
                break;
            }
            int count = buildFieldWidth(name, f, charsPerField, field);
            for (int i = 0; i < count && written + 1 < maxLen; ++i) {
                out[written++] = static_cast<char>(field[i]);
            }
            if (count < charsPerField) {
                break;  // ostatnie (niepelne) pole — koniec nazwy
            }
            if (written + 1 >= maxLen) {
                break;  // bufor wyjsciowy pelny
            }
        }
    }
    out[written] = '\0';
    return written;
}

uint8_t commandForField(int field, bool isDisc) {
    const bool lowField = (field <= 1);  // pola 0–1 vs 2–5
    if (isDisc) {
        return lowField ? CMD_DISC_LOW : CMD_DISC_HIGH;
    }
    return lowField ? CMD_TRACK_LOW : CMD_TRACK_HIGH;
}

} // namespace CdText
