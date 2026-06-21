#include "UnilinkParserModel.h"

#include "UnilinkFrame.h"

// =============================================================================
// UnilinkParserModel — implementacja czystego modelu parsera ramek UniLink.
// Odwzorowuje kryterium podstawowe UnilinkBus::readFrame: ciecie strumienia po
// granicach wyznaczonych przez CMD1 (rxBuffer[2]), bez wykrywania ciszy.
// =============================================================================

namespace UnilinkParserModel {

int parseNextFrame(const uint8_t* buf, int count, uint8_t* out, int maxLen) {
    if (buf == nullptr || out == nullptr || maxLen <= 0) {
        return 0;
    }
    // Granica ramki znana dopiero gdy mamy >= 3 bajty (RAD, TAD, CMD1).
    if (count < 3) {
        return 0;
    }
    const uint8_t cmd1 = buf[2];
    const int expected = UnilinkFrame::lengthFromCmd1(cmd1);
    if (expected <= 0 || count < expected) {
        return 0;  // brak jeszcze pelnej ramki wg dlugosci z CMD1
    }
    int n = expected;
    if (n > maxLen) n = maxLen;
    for (int i = 0; i < n; ++i) {
        out[i] = buf[i];
    }
    return n;
}

} // namespace UnilinkParserModel
