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

inline bool isExtEqual(const char* ext, size_t len, const char* target) {
    size_t tLen = 0;
    while (target[tLen] != '\0') tLen++;
    if (len != tLen) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = ext[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (c != target[i]) return false;
    }
    return true;
}

} // namespace

size_t sanitizeAscii(const char* in, char* out, size_t maxLen) {
    if (out == nullptr || maxLen == 0) {
        return 0;
    }
    if (in == nullptr || in[0] == '\0') {
        out[0] = '\0';
        return 0;
    }

    size_t totalInLen = 0;
    while (in[totalInLen] != '\0') totalInLen++;

    // 1. Wykryj ewentualne rozszerzenie pliku audio (.mp3, .wav, .flac itp.) na koncu
    size_t endLimit = totalInLen;
    for (size_t k = totalInLen; k > 0; --k) {
        if (in[k - 1] == '.') {
            const char* ext = &in[k];
            size_t extLen = totalInLen - k;
            if (isExtEqual(ext, extLen, "mp3") || isExtEqual(ext, extLen, "wav") ||
                isExtEqual(ext, extLen, "flac") || isExtEqual(ext, extLen, "m4a") ||
                isExtEqual(ext, extLen, "aac") || isExtEqual(ext, extLen, "ogg") ||
                isExtEqual(ext, extLen, "wma")) {
                endLimit = k - 1; // odetnij kropke i rozszerzenie
            }
            break;
        }
    }

    // 2. Pomin ewentualny prefiks numeryczny utworu (np. "01. ", "01 - ", "01_ ", "01 ")
    size_t start = 0;
    size_t digits = 0;
    while ((start + digits) < endLimit && in[start + digits] >= '0' && in[start + digits] <= '9') {
        digits++;
    }
    if (digits >= 1 && digits <= 3) {
        size_t afterDigits = start + digits;
        if (afterDigits < endLimit && (in[afterDigits] == '.' || in[afterDigits] == '-' || in[afterDigits] == '_')) {
            afterDigits++;
            while (afterDigits < endLimit && (in[afterDigits] == ' ' || in[afterDigits] == '-' || in[afterDigits] == '_')) {
                afterDigits++;
            }
            // Uzyj prefiksu tylko jesli po nim zostaje jeszcze jakis tekst
            if (afterDigits < endLimit) {
                start = afterDigits;
            }
        } else if (afterDigits < endLimit && in[afterDigits] == ' ') {
            while (afterDigits < endLimit && in[afterDigits] == ' ') {
                afterDigits++;
            }
            if (afterDigits < endLimit) {
                start = afterDigits;
            }
        }
    }

    // 3. Kopiowanie z zamiana polskich znakow UTF-8, podkreslen i redukcja podwojnych spacji
    size_t written = 0;
    bool lastWasSpace = true; // zapobiega poczatkowym spacjom

    for (size_t i = start; i < endLimit && in[i] != '\0' && written + 1 < maxLen; ) {
        unsigned char c1 = static_cast<unsigned char>(in[i]);
        
        // Obsługa UTF-8 (polskie znaki diakrytyczne)
        if (c1 >= 0xC0 && (i + 1) < endLimit && in[i+1] != '\0') {
            unsigned char c2 = static_cast<unsigned char>(in[i+1]);
            char replacement = 0;
            
            if (c1 == 0xC3) {
                if (c2 == 0xB3) replacement = 'o';      // ó
                else if (c2 == 0x93) replacement = 'O'; // Ó
            } else if (c1 == 0xC4) {
                if (c2 == 0x85) replacement = 'a';      // ą
                else if (c2 == 0x84) replacement = 'A'; // Ą
                else if (c2 == 0x87) replacement = 'c'; // ć
                else if (c2 == 0x86) replacement = 'C'; // Ć
                else if (c2 == 0x99) replacement = 'e'; // ę
                else if (c2 == 0x98) replacement = 'E'; // Ę
            } else if (c1 == 0xC5) {
                if (c2 == 0x82) replacement = 'l';      // ł
                else if (c2 == 0x81) replacement = 'L'; // Ł
                else if (c2 == 0x84) replacement = 'n'; // ń
                else if (c2 == 0x83) replacement = 'N'; // Ń
                else if (c2 == 0x9B) replacement = 's'; // ś
                else if (c2 == 0x9A) replacement = 'S'; // Ś
                else if (c2 == 0xBA) replacement = 'z'; // ź
                else if (c2 == 0xB9) replacement = 'Z'; // Ź
                else if (c2 == 0xBC) replacement = 'z'; // ż
                else if (c2 == 0xBB) replacement = 'Z'; // Ż
            }
            
            if (replacement != 0) {
                out[written++] = replacement;
                lastWasSpace = false;
                i += 2;
                continue;
            }
            
            // Pomiń inne wielobajtowe znaki UTF-8
            if ((c1 & 0xE0) == 0xC0) i += 2;
            else if ((c1 & 0xF0) == 0xE0) i += 3;
            else if ((c1 & 0xF8) == 0xF0) i += 4;
            else i += 1;
            continue;
        }

        // Zwykly znak ASCII
        char ch = static_cast<char>(c1);
        if (ch == '_' || ch == ' ') {
            if (!lastWasSpace && written + 1 < maxLen) {
                out[written++] = ' ';
                lastWasSpace = true;
            }
        } else if (isPrintableAscii(c1)) {
            out[written++] = ch;
            lastWasSpace = false;
        }
        i++;
    }

    // 4. Usun ewentualna spacje na koncu (trailing space)
    while (written > 0 && out[written - 1] == ' ') {
        written--;
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
