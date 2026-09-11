#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <Arduino.h>
#include <FS.h>

// =============================================================================
// Diagnostics — "czarna skrzynka" magistrali (flight recorder)
// =============================================================================
// Lekki bufor pierscieniowy ostatnich zdarzen na magistrali (ramki RX/TX,
// Slave Break). Nic nie wypisuje na biezaco — zrzuca caly bufor dopiero gdy
// poprosimy (dump()), np. w chwili wykrycia SYSTEM RESET radia. Dzieki temu
// widzimy DOKLADNIE co dzialo sie tuz przed resetem, bez ciaglego spamu w
// konsoli (ktory sam zaburza timing magistrali).
//
// Rejestrowanie jest tanie (memcpy max 16 bajtow) i wywolywane wylacznie z
// kontekstu petli glownej — nigdy z ISR.
// =============================================================================

namespace Diagnostics {

// Zapisz ramke do bufora. label: krotki znacznik ("RX", "TX").
void recordFrame(const char* label, const uint8_t* data, int len);

// Zapisz zdarzenie tekstowe (np. "BREAK").
void recordNote(const char* note);

// Wypisz caly bufor (od najstarszego do najnowszego) z czasami wzglednymi.
void dump(const char* reason);

// Zapisz bufor surowych ramek bezposrednio do otwartego pliku (pendrive).
// Uzycie: Diagnostics::dumpToFile(crashFile) w WiFiLogger::dumpCrashLog().
void dumpToFile(fs::File& f);

// Migawka w RAM (czarna skrzynka zamrozona w chwili zdarzenia bez blokowania I/O)
void captureSnapshot();
bool hasSnapshot();
void clearSnapshot();
void dumpSnapshotToFile(fs::File& f);

// --- SYSTEM ERROR MANAGEMENT (DLA CD-TEXT) ---
enum class SystemError : uint8_t {
    None = 0,
    DacPinShort,    // "PIN X VCC"
    DacInitErr,     // "DAC FAIL"
    AudioTaskErr,   // "I2S TASK"
    UsbHostErr,     // "USB FAIL"
    NoUsb,          // "NO PENDRIVE"
    UsbFsErr,       // "FAT32 ERR"
    NoTracks,       // "NO TRACKS"
    EmptyDisc,      // "EMPTY CD"
    FileErr         // "BAD FILE"
};

void setError(SystemError err, int detail = 0);
void clearError(SystemError err);
SystemError getError();
bool hasError();
const char* getErrorString();
const char* getErrorDiscName();

} // namespace Diagnostics

#endif // DIAGNOSTICS_H
