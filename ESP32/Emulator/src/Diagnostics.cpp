// =============================================================================
// Diagnostics.cpp — implementacja "czarnej skrzynki" magistrali
// =============================================================================
#include "Diagnostics.h"
#include "Config.h"   // Serial -> WiFiLogger: komunikaty trafiaja tez do czarnej skrzynki

namespace Diagnostics {

constexpr int RING_SIZE     = 128;   // ile ostatnich zdarzen pamietamy
constexpr int MAX_FRAME_LEN = 16;   // najdluzsza ramka UniLink

struct Entry {
    unsigned long timeMs;
    const char*   label;
    uint8_t       len;
    uint8_t       data[MAX_FRAME_LEN];
};

static Entry ring[RING_SIZE];
static int   head  = 0;     // indeks nastepnego wpisu
static int   count = 0;     // ile wpisow zapisano (do RING_SIZE)
static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

void recordFrame(const char* label, const uint8_t* data, int len) {
    if (len < 0 || data == nullptr) len = 0;
    if (len > MAX_FRAME_LEN) len = MAX_FRAME_LEN;
    const unsigned long t = millis();
    portENTER_CRITICAL(&s_ringMux);
    Entry& e = ring[head];
    e.timeMs = t;
    e.label  = label;
    e.len = (uint8_t)len;
    for (int i = 0; i < len; i++) e.data[i] = data[i];
    head = (head + 1) % RING_SIZE;
    if (count < RING_SIZE) count++;
    portEXIT_CRITICAL(&s_ringMux);
}

void recordNote(const char* note) {
    recordFrame(note, nullptr, 0);
}

void dump(const char* reason) {
    Serial.printf("\n===== CZARNA SKRZYNKA (%s) — ostatnie %d zdarzen =====\n",
                  reason, count);
    unsigned long now = millis();
    int idx = (head - count + RING_SIZE) % RING_SIZE;
    for (int i = 0; i < count; i++) {
        Entry& e = ring[idx];
        Serial.printf("[-%5lums] %-5s", now - e.timeMs, e.label);
        for (int b = 0; b < e.len; b++) {
            Serial.printf(" %02X", e.data[b]);
        }
        Serial.println();
        idx = (idx + 1) % RING_SIZE;
    }
    Serial.println("======================================================\n");
}

void dumpToFile(fs::File& f) {
    f.printf("===== BUS FRAMES (last %d) =====\n", count);
    unsigned long now = millis();
    int idx = (head - count + RING_SIZE) % RING_SIZE;
    for (int i = 0; i < count; i++) {
        Entry& e = ring[idx];
        f.printf("[-%5lums] %-8s", now - e.timeMs, e.label);
        for (int b = 0; b < e.len; b++) {
            f.printf(" %02X", e.data[b]);
        }
        f.print('\n');
        idx = (idx + 1) % RING_SIZE;
    }
    f.println("=====================================");
}

// --- MIGAWEK RAM DLA ODROCZONEGO ZRZUTU ---
static Entry snapshotRing[RING_SIZE];
static int   snapshotHead = 0;
static int   snapshotCount = 0;
static unsigned long snapshotNow = 0;
static bool  s_hasSnapshot = false;

void captureSnapshot() {
    if (s_hasSnapshot) return; // zachowaj pierwsze zdarzenie z sesji
    snapshotNow = millis();
    portENTER_CRITICAL(&s_ringMux);
    snapshotHead = head;
    snapshotCount = count;
    portEXIT_CRITICAL(&s_ringMux);
    // Porcjami: dluga sekcja krytyczna opoznilaby przerwanie zegara magistrali
    // (ten sam rdzen) i przeklamala bity.
    constexpr int CHUNK = 16;
    static_assert(RING_SIZE % CHUNK == 0, "RING_SIZE musi byc wielokrotnoscia CHUNK");
    for (int i = 0; i < RING_SIZE; i += CHUNK) {
        portENTER_CRITICAL(&s_ringMux);
        memcpy(&snapshotRing[i], &ring[i], CHUNK * sizeof(Entry));
        portEXIT_CRITICAL(&s_ringMux);
    }
    s_hasSnapshot = true;
}

bool hasSnapshot() {
    return s_hasSnapshot;
}

void clearSnapshot() {
    s_hasSnapshot = false;
    snapshotCount = 0;
}

void dumpSnapshotToFile(fs::File& f) {
    f.printf("===== BUS FRAMES (last %d) =====\n", snapshotCount);
    int idx = (snapshotHead - snapshotCount + RING_SIZE) % RING_SIZE;
    for (int i = 0; i < snapshotCount; i++) {
        Entry& e = snapshotRing[idx];
        f.printf("[-%5lums] %-8s", snapshotNow - e.timeMs, e.label);
        for (int b = 0; b < e.len; b++) {
            f.printf(" %02X", e.data[b]);
        }
        f.print('\n');
        idx = (idx + 1) % RING_SIZE;
    }
    f.println("=====================================");
}

static SystemError currentError = SystemError::None;
static int errorDetail = 0;
static char errorStr[32] = "";

void setError(SystemError err, int detail) {
    if (currentError != err || errorDetail != detail) {
        currentError = err;
        errorDetail = detail;
        switch (err) {
            case SystemError::None:
                errorStr[0] = '\0';
                break;
            case SystemError::DacPinShort:
                snprintf(errorStr, sizeof(errorStr), (detail >= 10) ? "PIN%d VCC" : "PIN %d VCC", detail);
                break;
            case SystemError::DacInitErr:
                snprintf(errorStr, sizeof(errorStr), "DAC FAIL");
                break;
            case SystemError::AudioTaskErr:
                snprintf(errorStr, sizeof(errorStr), "I2S TASK");
                break;
            case SystemError::UsbHostErr:
                snprintf(errorStr, sizeof(errorStr), "USB FAIL");
                break;
            case SystemError::NoUsb:
                snprintf(errorStr, sizeof(errorStr), "NO PENDRIVE");
                break;
            case SystemError::UsbFsErr:
                snprintf(errorStr, sizeof(errorStr), "FAT32 ERR");
                break;
            case SystemError::NoTracks:
                snprintf(errorStr, sizeof(errorStr), "NO TRACKS");
                break;
            case SystemError::EmptyDisc:
                snprintf(errorStr, sizeof(errorStr), "EMPTY CD");
                break;
            case SystemError::FileErr:
                snprintf(errorStr, sizeof(errorStr), "BAD FILE");
                break;
        }
        if (err != SystemError::None) {
            Serial.printf("[Diagnostics] BŁĄD SYSTEMOWY: %s\n", errorStr);
        }
    }
}

void clearError(SystemError err) {
    if (currentError == err) {
        currentError = SystemError::None;
        errorDetail = 0;
        errorStr[0] = '\0';
        Serial.println("[Diagnostics] Błąd systemowy ustąpił (OK).");
    }
}

SystemError getError() {
    return currentError;
}

bool hasError() {
    return currentError != SystemError::None;
}

const char* getErrorString() {
    return errorStr;
}

const char* getErrorDiscName() {
    return "ERROR";
}

} // namespace Diagnostics
