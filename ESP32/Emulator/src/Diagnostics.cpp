// =============================================================================
// Diagnostics.cpp — implementacja "czarnej skrzynki" magistrali
// =============================================================================
#include "Diagnostics.h"

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

void recordFrame(const char* label, const uint8_t* data, int len) {
    Entry& e = ring[head];
    e.timeMs = millis();
    e.label  = label;
    if (len < 0) len = 0;
    if (len > MAX_FRAME_LEN) len = MAX_FRAME_LEN;
    e.len = (uint8_t)len;
    for (int i = 0; i < len; i++) e.data[i] = data[i];

    head = (head + 1) % RING_SIZE;
    if (count < RING_SIZE) count++;
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

} // namespace Diagnostics
