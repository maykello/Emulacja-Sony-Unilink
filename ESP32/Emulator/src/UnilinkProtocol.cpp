// =============================================================================
// UnilinkProtocol.cpp — zmieniarka CD na magistrali Sony Unilink (model OE)
// =============================================================================
#include "UnilinkProtocol.h"
#include "AudioPlayer.h"
#include "CdChanger.h"
#include "CdText.h"
#include "Config.h"
#include "Diagnostics.h"
#include "Magazine.h"
#include "TxQueue.h"
#include "UnilinkBus.h"
#include "UnilinkFrame.h"
#include <Preferences.h>

namespace UnilinkProtocol {

namespace {

using MechState  = CdChanger::MechState;
using RepeatMode = CdChanger::RepeatMode;
using UnilinkBus::Frame;

// Maska w arbitrazu 01 15 = dolny nibbel CMD2 ramki appoint (31 10 02 14 -> 0x04).
constexpr uint8_t CLAIM_MASK_DEFAULT = 0x04;

// Odpowiedz na ANYONE? zanim radio przydzieli adres (sniff OE):
//   10 30 8C D0 9C 05 A8 1F A3 0B 00
// Na appoint ta sama ramka z przydzielonym adresem; D1 dobrane tak, by Parity2
// zawsze wynosilo 0x0B (10 31 8C D0 9D 04 A8 1F A3 0B 00).
void buildDeviceInfo(uint8_t addr, uint8_t* f)
{
    f[0] = ADDR_MASTER;
    f[1] = addr;
    f[2] = 0x8C;
    f[3] = 0xD0;
    f[4] = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    f[5] = (uint8_t)(0xA1 - f[4]);
    f[6] = 0xA8;
    f[7] = 0x1F;
    f[8] = 0xA3;
    f[9] = UnilinkFrame::parity2(f[4], &f[5], 4);
    f[10] = 0x00;
}

// ---------------------------------------------------------------------------
// Stan sesji
// ---------------------------------------------------------------------------
TaskHandle_t s_task = nullptr;
Preferences  s_prefs;

volatile bool    s_appointed = false;
volatile uint8_t s_myAddr    = ADDR_GROUP_CD;
uint8_t          s_claimMask = CLAIM_MASK_DEFAULT;
// Adres z poprzedniej sesji (NVS). Dopoki radio nie przydzieli adresu, odpowiadamy
// na ping pod nim — radio po wlaczeniu czesto pinguje znana zmieniarke zamiast
// robic discovery od nowa.
uint8_t       s_nvsAddr          = 0;
bool          s_dualListenWaited = false;
unsigned long s_appointMs        = 0;
unsigned long s_anyoneAnsweredMs = 0;
unsigned long s_magicSentMs      = 0;
uint8_t       s_magicStreak      = 0;
bool          s_selected         = false;   // radio wybralo nas jako zrodlo (startujemy false, czekamy na F0 31, 87 ON lub komende)
bool          s_displayYielded   = false;   // inne urzadzenie (np. Tuner TA/ekran) przejelo 0x70

volatile bool s_persistAddrPending = false;
uint8_t       s_savedAddr  = 0;
uint8_t       s_savedMask  = 0;
bool          s_savedAlloc = false;

// BUS_ON po filtracji
volatile bool s_busActive     = false;
bool          s_busRawLast    = false;
unsigned long s_busEdgeMs     = 0;
unsigned long s_busOffSinceMs = 0;
unsigned long s_busOnSinceMs  = 0;
bool          s_busOffStopped = false;

// Aktywnosc radia
unsigned long          s_lastFrameMs    = 0;
volatile unsigned long s_lastOwnPingMs  = 0;
bool                   s_silentReported = false;

// ---------------------------------------------------------------------------
// Nadawanie: kolejka + slot na grant
// ---------------------------------------------------------------------------
Tx::TxQueue s_txQueue;

struct DisplayKey {
    uint8_t disc;
    uint8_t track;
    uint8_t status;
    uint8_t minutes;   // 0xFF = czas nieznany ("--:--")
    uint8_t seconds;
    uint8_t flags;     // dolny nibbel flag przy numerze plyty (0x8 / 0xB / 0xA)
    bool    valid;
};
// Co radio faktycznie ma na ekranie (ostatnia DOSTARCZONA ramka ekranu).
DisplayKey s_shown = { 0, 0, 0xFF, 0xFF, 0xFF, 0, false };

enum class SlotKind : uint8_t { None, Queued, Display };
SlotKind      s_slotKind     = SlotKind::None;
DisplayKey    s_slotKey      = { 0, 0, 0xFF, 0xFF, 0xFF, 0, false };
bool          s_slotCarriesScreen = false;   // dostarczenie aktualizuje s_shown
bool          s_slotIsStatusC0    = false;   // true dla pelnej ramki 0xC0/0x8E, false dla 0x90
uint32_t      s_slotClaimBase = 0;           // claimsSent() w chwili zaladowania slotu
uint32_t      s_grantsSeen   = 0;
uint32_t      s_claimsSeen   = 0;
unsigned long s_lastClaimMs  = 0;
unsigned long s_lastTimeFrameMs = 0;
bool          s_justEndedSeek   = false;

unsigned long s_lastPoll15Ms      = 0;

// Slave Break
unsigned long s_nextBreakMs       = 0;
bool          s_breakAwaitingPoll = false;
unsigned long s_breakDoneMs       = 0;
uint8_t       s_breakFailStreak   = 0;
uint8_t       s_noWaveStreak      = 0;

// Statystyki [STAT]
struct ProtoStats {
    uint16_t pings, poll15, claims, grants, breaks, breakNoPoll, breakNoWave, bad;
};
ProtoStats    s_stat = {};
unsigned long s_lastStatMs = 0;

// ---------------------------------------------------------------------------
// Nibbel flag przy numerze plyty (D4 w 0x90, D9 w 0xC0). Pelny cykl w sniffie
// prawdziwej zmieniarki (CD8, 144705.txt linie 4438..4933):
//   etap 0 -> 0x8 (0x88): start utworu, wysylane pierwsze C0 i pierwszy blok D2+DA
//   etap 1 -> 0xB (0x8B): wysylane drugie C0 i powtorzony blok D2+DA+D7
//   etap 2 -> 0xA (0x8A): wysylane trzecie C0, po ktorym ekran (0x71) prosi o 84 D7
// ---------------------------------------------------------------------------
uint8_t s_textFlagState = 0;   // 0 = 0x8, 1 = 0xB, 2 = 0xA
uint8_t s_textPassSent  = 0;   // 0 = nic nie poszlo, 1 = poszedl pass 1 (0x8), 2 = poszedl pass 2 (0xB)

uint8_t discFlagsNibble()
{
    if (audioGetTrackCount(CdChanger::disk()) == 0 && !Diagnostics::hasError()) return 0x00;
    uint8_t flags = 0x08;
    if (s_textFlagState >= 1) flags |= 0x02;
    if (s_textFlagState == 1) flags |= 0x01;
    return flags;
}

uint8_t decodeFpad(uint8_t v)
{
    return ((v & 0xF0) == 0xF0) ? (uint8_t)(v & 0x0F) : UnilinkFrame::decodeBcd(v);
}

DisplayKey keyNow()
{
    DisplayKey k;
    const MechState ms   = CdChanger::mechState();
    const bool timeKnown = (ms == MechState::Playing || ms == MechState::Seeking);
    k.disc    = CdChanger::disk();
    k.track   = CdChanger::track();
    k.status  = UnilinkFrame::statusByte(ms);
    k.minutes = timeKnown ? CdChanger::minutes() : 0xFF;
    k.seconds = timeKnown ? CdChanger::seconds() : 0xFF;
    k.flags   = discFlagsNibble();
    k.valid   = true;
    return k;
}

DisplayKey keyFromC0(const uint8_t* b)
{
    DisplayKey k;
    k.status  = b[3];
    k.track   = decodeFpad(b[10]);
    k.minutes = (b[11] == 0xFF) ? 0xFF : decodeFpad(b[11]);
    k.seconds = (b[12] == 0xFF) ? 0xFF : UnilinkFrame::decodeBcd(b[12]);
    k.disc    = (uint8_t)(b[13] >> 4);
    k.flags   = (uint8_t)(b[13] & 0x0F);
    k.valid   = true;
    return k;
}

// ---------------------------------------------------------------------------
// Ramki ekranu
// ---------------------------------------------------------------------------
// 0xC0 — pelny status, 1:1 ze sniffu CDX-M670:
//   70 31 C0 00 61 00 00 00 00 30 F1 F0 00 88 FA 00   (gra, TR1 0:00, CD8)
//   77 31 C0 20 88 00 00 00 00 30 F2 FF FF 88 30 00   (zmiana utworu, czas nieznany)
//   77 31 C0 40 A8 00 00 00 00 00 F1 F0 00 81 1A 00   (zmiana plyty)
void buildStatusC0(uint8_t* f)
{
    const MechState ms   = CdChanger::mechState();
    const uint8_t   disc = CdChanger::disk();
    const bool timeKnown = (ms == MechState::Playing || ms == MechState::Seeking);
    const bool discPresent = (audioGetTrackCount(disc) > 0) || Diagnostics::hasError();
    const bool changingDisc = (ms == MechState::LoadingTrack);

    f[0]  = (ms == MechState::ChangedCd || ms == MechState::LoadingTrack) ? 0x77 : 0x70;
    f[1]  = s_myAddr;
    f[2]  = 0xC0;
    f[3]  = UnilinkFrame::statusByte(ms);
    f[4]  = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    f[5]  = 0x00;
    f[6]  = 0x00;
    f[7]  = 0x00;
    f[8]  = 0x00;
    f[9]  = (!changingDisc && discPresent) ? 0x30 : 0x00;
    f[10] = UnilinkFrame::encodeBcdFpad(CdChanger::track());
    f[11] = timeKnown ? UnilinkFrame::encodeBcdFpad(CdChanger::minutes())
                      : (changingDisc ? 0xF0 : 0xFF);
    f[12] = timeKnown ? UnilinkFrame::encodeBcd(CdChanger::seconds())
                      : (changingDisc ? 0x00 : 0xFF);
    f[13] = UnilinkFrame::discHighNibble(disc, changingDisc ? (discPresent ? 0x01 : 0x00) : discFlagsNibble());
    f[14] = UnilinkFrame::parity2(f[4], &f[5], 9);
    f[15] = 0x00;
}

// 0x90 — czas odtwarzania wysylany co sekunde: 70 31 90 30 61 F1 F0 05 8A D1 00
void buildTimeTick90(uint8_t* f)
{
    f[0]  = 0x70;
    f[1]  = s_myAddr;
    f[2]  = 0x90;
    f[3]  = 0x30;
    f[4]  = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    f[5]  = UnilinkFrame::encodeBcdFpad(CdChanger::track());
    f[6]  = UnilinkFrame::encodeBcdFpad(CdChanger::minutes());
    f[7]  = UnilinkFrame::encodeBcd(CdChanger::seconds());
    f[8]  = UnilinkFrame::discHighNibble(CdChanger::disk(), discFlagsNibble());
    f[9]  = UnilinkFrame::parity2(f[4], &f[5], 4);
    f[10] = 0x00;
}

// 0x8E — ekran zmieniarki w spoczynku: 70 31 8E 80 AF 90 00 00 A0 DF 00
void buildIdle8E(uint8_t* f)
{
    f[0]  = 0x70;
    f[1]  = s_myAddr;
    f[2]  = 0x8E;
    f[3]  = 0x80;
    f[4]  = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    f[5]  = 0x90;
    f[6]  = 0x00;
    f[7]  = 0x00;
    f[8]  = UnilinkFrame::discHighNibble(CdChanger::disk(), 0x00);
    f[9]  = UnilinkFrame::parity2(f[4], &f[5], 4);
    f[10] = 0x00;
}

bool textQueued();

bool screenChanged(const DisplayKey& k)
{
    if (!s_shown.valid) return true;
    if (k.disc != s_shown.disc || k.track != s_shown.track || k.status != s_shown.status) return true;
    if (k.status == 0x00 && s_shown.seconds == 0xFF && k.seconds != 0xFF) return true;
    // Przejscie flag CD-TEXT (0x8 -> 0xB -> 0xA) w stanie Playing wymaga wyslania
    // pelnej ramki 0xC0, gdy poprzedni blok ramek tekstowych zszedl juz z kolejki.
    if (k.status == 0x00 && !CdChanger::isSeeking() && !textQueued() && k.flags != s_shown.flags) return true;
    return false;
}

int buildDisplayFrame(uint8_t* f, DisplayKey& key)
{
    key = keyNow();
    const MechState ms = CdChanger::mechState();
    if (ms == MechState::Idle || ms == MechState::Init) {
        buildIdle8E(f);
        key.minutes = 0xFF;
        key.seconds = 0xFF;
        return 11;
    }
    // Pelny 0xC0 przy zmianie plyty/utworu/stanu/flag CD-TEXT i w stanach przejsciowych.
    // FF/REW: tylko 0x90 (czas plynie, ekran sie nie przeladowuje).
    if (!CdChanger::isSeeking() &&
        (screenChanged(key) || ms == MechState::LoadingTrack || ms == MechState::ChangedCd)) {
        buildStatusC0(f);
        key = keyFromC0(f);
        return 16;
    }
    buildTimeTick90(f);
    return 11;
}

bool needDisplayFrame(unsigned long now)
{
    if (!s_selected || s_displayYielded) return false;
    const MechState ms = CdChanger::mechState();
    if (ms == MechState::Init) return false;
    if (ms == MechState::Idle) {
        // Jednorazowa ramka 0x8E przy wejsciu w Idle (lub na starcie, aby ekran nie byl pusty)
        return !s_shown.valid || s_shown.status != 0x80;
    }
    const DisplayKey k = keyNow();
    if (!CdChanger::isSeeking() && screenChanged(k)) return true;
    if (ms == MechState::Playing || ms == MechState::Seeking) {
        if (s_justEndedSeek) return true;
        if (k.minutes != s_shown.minutes || k.seconds != s_shown.seconds) {
            const unsigned long minGap = (ms == MechState::Seeking) ? SEEK_TICK_MIN_MS : TIME_TICK_MIN_MS;
            return (now - s_lastTimeFrameMs) >= minGap;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Kolejka ramek
// ---------------------------------------------------------------------------
void enqueueFrame(uint8_t priority, const uint8_t* bytes, int len)
{
    if (bytes == nullptr || len <= 0) return;
    if (len > Tx::TX_ITEM_MAX_BYTES) len = Tx::TX_ITEM_MAX_BYTES;
    s_txQueue.enqueue(priority, bytes, (uint8_t)len);
}

// ---------------------------------------------------------------------------
// CD-TEXT (wariant CDX-M670: 0xD2 utwor, 0xDA plyta, 0xD7 koniec bloku)
// ---------------------------------------------------------------------------
constexpr int TEXT_SLOTS = 8;   // CMD2 + D1..D7

void trackTextNow(char* out, size_t len)
{
    const RepeatMode rep = CdChanger::repeatMode();
    if (Diagnostics::hasError() && rep == RepeatMode::Off) {
        snprintf(out, len, "%s", Diagnostics::getErrorString());
    } else if (rep == RepeatMode::One) {
        snprintf(out, len, "%lu mBar", 1500UL + ((millis() / 500) % 11) * 100UL);
    } else if (rep == RepeatMode::All) {
        snprintf(out, len, "%lu mg", (unsigned long)((millis() / 500) % 54));
    } else {
        audioGetTrackName(CdChanger::disk(), CdChanger::track(), out, len);
    }
}

void discTextFor(uint8_t disc, char* out, size_t len)
{
    const RepeatMode rep = CdChanger::repeatMode();
    if (Diagnostics::hasError() && rep == RepeatMode::Off) {
        snprintf(out, len, "%s", Diagnostics::getErrorDiscName());
    } else if (rep != RepeatMode::Off) {
        snprintf(out, len, "OBD");
    } else {
        audioGetDiscName(disc, out, len);
    }
}

int buildTextSegment(const char* name, int offset, bool last, uint8_t* slots)
{
    for (int i = 0; i < TEXT_SLOTS; ++i) slots[i] = 0x00;
    int used = 0;
    while (used < 6 && name[offset + used] != '\0') {
        slots[used] = (uint8_t)name[offset + used];
        used++;
    }
    if (last) {
        slots[6] = 0x00;
        slots[7] = 0x01;
    } else {
        slots[6] = 0x02;
        slots[7] = 0x00;
    }
    return used;
}

void enqueueTextFrame(uint8_t cmd1, const uint8_t* slots, uint8_t d8, uint8_t d9 = 0x10)
{
    uint8_t f[16];
    f[0] = 0x70;
    f[1] = s_myAddr;
    f[2] = cmd1;
    f[3] = slots[0];
    f[4] = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    for (int i = 1; i < TEXT_SLOTS; ++i) f[4 + i] = slots[i];
    f[12] = d8;
    f[13] = d9;
    f[14] = UnilinkFrame::parity2(f[4], &f[5], 9);
    f[15] = 0x00;
    enqueueFrame(Tx::PRIO_CD_TEXT, f, sizeof(f));
}

void enqueueTextEnd()
{
    const uint8_t slots[TEXT_SLOTS] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00 };
    enqueueTextFrame(0xD7, slots, 0x00, 0x1C);
}

void enqueueTextName(uint8_t cmd1, const char* rawName, uint8_t d8)
{
    char sane[64];
    CdText::sanitizeAscii(rawName, sane, sizeof(sane));
    int total = 0;
    while (sane[total] != '\0' && total < CDTEXT_D2_MAX_CHARS) total++;
    sane[total] = '\0';

    uint8_t slots[TEXT_SLOTS];
    if (total == 0) {
        buildTextSegment(sane, 0, false, slots);
        enqueueTextFrame(cmd1, slots, d8);
        return;
    }
    int offset = buildTextSegment(sane, 0, false, slots);
    enqueueTextFrame(cmd1, slots, d8);
    while ((total - offset) > 6) {
        offset += buildTextSegment(sane, offset, false, slots);
        enqueueTextFrame(cmd1, slots, d8);
    }
    buildTextSegment(sane, offset, true, slots);
    enqueueTextFrame(cmd1, slots, d8);
}

void enqueueDiscIndicator(uint8_t disc)
{
    char raw[64];
    discTextFor(disc, raw, sizeof(raw));
    char sane[64];
    CdText::sanitizeAscii(raw, sane, sizeof(sane));
    uint8_t slots[TEXT_SLOTS];
    if (sane[0] == '\0') {
        for (int i = 0; i < TEXT_SLOTS; ++i) slots[i] = 0x00;
        slots[6] = 0x02;
    } else {
        buildTextSegment(sane, 0, true, slots);
    }
    enqueueTextFrame(0xDA, slots, (uint8_t)(disc & 0x0F));
}

bool textQueued() { return s_txQueue.countPriority(Tx::PRIO_CD_TEXT) > 0; }

bool enqueueTrackTextBlock(bool withEndMarker, bool force)
{
    if (textQueued()) {
        if (!force) return false;
        s_txQueue.dropPriority(Tx::PRIO_CD_TEXT);
    }
    char raw[64];
    trackTextNow(raw, sizeof(raw));
    enqueueTextName(0xD2, raw, UnilinkFrame::encodeBcdFpad(CdChanger::track()));
    enqueueDiscIndicator(CdChanger::disk());
    if (withEndMarker) enqueueTextEnd();
    return true;
}

bool enqueueTrackTextOnly()
{
    if (textQueued()) s_txQueue.dropPriority(Tx::PRIO_CD_TEXT);
    char raw[64];
    trackTextNow(raw, sizeof(raw));
    enqueueTextName(0xD2, raw, UnilinkFrame::encodeBcdFpad(CdChanger::track()));
    enqueueTextEnd();
    return true;
}

void enqueueDiscTextBlock(uint8_t disc)
{
    if (textQueued()) s_txQueue.dropPriority(Tx::PRIO_CD_TEXT);
    char raw[64];
    discTextFor(disc, raw, sizeof(raw));
    enqueueTextName(0xDA, raw, (uint8_t)(disc & 0x0F));
    enqueueTextEnd();
}

void enqueueTextField8(bool isDisc, int field)
{
    if (field < 0 || field > CdText::MAX_FIELD) return;
    char raw[64];
    if (isDisc) discTextFor(CdChanger::disk(), raw, sizeof(raw));
    else        trackTextNow(raw, sizeof(raw));
    char name[64];
    CdText::sanitizeAscii(raw, name, sizeof(name));
    if (!CdText::fieldExists(name, field, CdText::FIELD8_CHARS)) return;

    uint8_t chars[CdText::FIELD8_CHARS];
    CdText::buildField8(name, field, chars);
    uint8_t f[16];
    f[0] = 0x70;
    f[1] = s_myAddr;
    f[2] = CdText::commandForField(field, isDisc);
    f[3] = chars[0];
    f[4] = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    for (int i = 1; i < CdText::FIELD8_CHARS; ++i) f[4 + i] = chars[i];
    f[12] = 0x00;
    f[13] = (uint8_t)field;
    f[14] = UnilinkFrame::parity2(f[4], &f[5], 9);
    f[15] = 0x00;
    enqueueFrame(Tx::PRIO_CD_TEXT, f, sizeof(f));
}

uint8_t discFromRequest(const Frame& fr)
{
    uint8_t disc = CdChanger::disk();
    if (fr.len >= 11 && fr.data[5] != 0x00) {
        const uint8_t d1 = fr.data[5];
        const uint8_t cand = ((d1 & 0xF0) == 0xF0) ? UnilinkFrame::discNibbleToNumber(d1) : d1;
        if (cand >= 1 && cand <= MAX_DISC) disc = cand;
    }
    return disc;
}

// --- CD-TEXT wysylany sam z siebie po zmianie utworu/plyty -------------------
uint8_t                   s_textSentDisc   = 0;
uint8_t                   s_textSentTrack  = 0;
RepeatMode                s_textSentRepeat = RepeatMode::Off;
Diagnostics::SystemError  s_textSentError  = Diagnostics::SystemError::None;
bool                      s_wasSeeking     = false;
unsigned long             s_lastObdMs      = 0;

void resetCdTextCache()
{
    s_textSentDisc   = 0;
    s_textSentTrack  = 0;
    s_textSentRepeat = RepeatMode::Off;
    s_textSentError  = (Diagnostics::SystemError)255;
    s_textFlagState  = 0;
    s_textPassSent   = 0;
}

void serviceCdText(unsigned long now)
{
    if (!s_appointed || !s_selected || s_displayYielded) return;
    const MechState ms = CdChanger::mechState();
    if (ms == MechState::Init || ms == MechState::Idle) return;
    if (ms == MechState::Seeking) {
        s_wasSeeking = true;
        return;
    }
    if (s_wasSeeking) {
        s_wasSeeking = false;
    }

    const uint8_t disc   = CdChanger::disk();
    const uint8_t track  = CdChanger::track();
    const RepeatMode rep = CdChanger::repeatMode();
    const Diagnostics::SystemError err = Diagnostics::getError();

    const bool changed = (disc != s_textSentDisc || track != s_textSentTrack ||
                          rep != s_textSentRepeat || err != s_textSentError);
    if (changed) {
        // Nowy utwor/plyta: zaczynamy cykl flag 0x8 -> 0xB -> 0xA od stanu 0 (0x8),
        // zanim zbudowana zostanie pierwsza ramka 0xC0 0x00 dla tego utworu.
        s_textSentDisc   = disc;
        s_textSentTrack  = track;
        s_textSentRepeat = rep;
        s_textSentError  = err;
        s_textFlagState  = 0;
        s_textPassSent   = 0;
    }

    if (ms != MechState::Playing) return;
    if (!s_shown.valid || s_shown.status != 0x00 || s_shown.seconds == 0xFF) return;
    if (s_shown.disc != disc || s_shown.track != track) return;

    // 3-etapowa sekwencja OEM ze sniffu 144705.txt:
    //   1) Po dostarczeniu C0 z flaga 0x8: wyslij D2+DA i przelacz flage na 0xB (stan 1).
    //   2) Gdy kolejka D2+DA zejdzie, screenChanged() wysle C0 z flaga 0xB.
    //      Po dostarczeniu C0 z flaga 0xB: wyslij D2+DA+D7 i przelacz flage na 0xA (stan 2).
    //   3) Gdy kolejka zejdzie, screenChanged() wysle C0 z flaga 0xA (na co 0x71 odpowiada 84 D7).
    if (s_textPassSent == 0 && s_shown.flags == 0x08) {
        if (!enqueueTrackTextBlock(false, true)) return;
        s_textPassSent  = 1;
        s_textFlagState = 1; // nastepny 0xC0 po zejsciu kolejki dostanie flage 0x0B
        char name[64];
        char discName[64];
        trackTextNow(name, sizeof(name));
        discTextFor(disc, discName, sizeof(discName));
        Serial.printf(">> CD-TEXT: CD%u TR%u plyta=\"%s\" utwor=\"%s\"\n",
                      disc, track, discName, name);
        return;
    }

    if (s_textPassSent == 1 && s_shown.flags == 0x0B && !textQueued()) {
        if (!enqueueTrackTextBlock(true, true)) return;
        s_textPassSent  = 2;
        s_textFlagState = 2; // nastepny 0xC0 po zejsciu kolejki dostanie flage 0x0A
        return;
    }

    if (s_textPassSent >= 2 && rep != RepeatMode::Off && now - s_lastObdMs >= OBD_UPDATE_INTERVAL_MS) {
        s_lastObdMs = now;
        enqueueTrackTextOnly();
    }
}

// --- Magazynek / identyfikator plyty / ikony ---------------------------------
void enqueueMiddle(uint8_t rad, uint8_t cmd1, uint8_t cmd2, const uint8_t* d, uint8_t prio)
{
    uint8_t f[11];
    f[0]  = rad;
    f[1]  = s_myAddr;
    f[2]  = cmd1;
    f[3]  = cmd2;
    f[4]  = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    f[5]  = d[0];
    f[6]  = d[1];
    f[7]  = d[2];
    f[8]  = d[3];
    f[9]  = UnilinkFrame::parity2(f[4], &f[5], 4);
    f[10] = 0x00;
    enqueueFrame(prio, f, sizeof(f));
}

void enqueueDiscId(uint8_t disc, bool discChangeVariant)
{
    uint8_t d[Magazine::DISC_ID_DATA_LEN];
    uint8_t cmd2 = 0x00;
    Magazine::buildDiscId(disc, discChangeVariant, cmd2, d);
    uint8_t f[16];
    f[0] = ADDR_MASTER;
    f[1] = s_myAddr;
    f[2] = discChangeVariant ? 0xD5 : 0xC5;
    f[3] = cmd2;
    f[4] = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    for (int i = 0; i < Magazine::DISC_ID_DATA_LEN; ++i) f[5 + i] = d[i];
    f[14] = UnilinkFrame::parity2(f[4], &f[5], Magazine::DISC_ID_DATA_LEN);
    f[15] = 0x00;
    enqueueFrame(Tx::PRIO_DISC_ID, f, sizeof(f));
}

void onDiscChanged(uint8_t disc)
{
    const bool present = audioGetTrackCount(disc) > 0 || Diagnostics::hasError();
    const uint8_t d[4] = { 0x00, 0x00, 0x00,
                           UnilinkFrame::discHighNibble(disc, present ? 0x08 : 0x00) };
    enqueueMiddle(0x90, 0x9C, 0x00, d, Tx::PRIO_STATUS);
    enqueueDiscId(disc, true);
}

void enqueueMagazineMap()
{
    const uint16_t pm = Magazine::presenceMap();
    const uint8_t d[4] = {
        Magazine::magazineD1FromMap(pm), 0x00, 0x00,
        UnilinkFrame::discHighNibble(CdChanger::disk(), MAX_DISC),
    };
    enqueueMiddle(0x70, 0x95, Magazine::magazineCmd2FromMap(pm), d, Tx::PRIO_MAGAZINE);
}

void enqueueDiscInfo(uint8_t disc)
{
    uint8_t d[Magazine::DISC_INFO_DATA_LEN];
    Magazine::buildDiscInfo(disc, d);
    enqueueMiddle(0x70, 0x97, 0x01, d, Tx::PRIO_MAGAZINE);
}

// ---------------------------------------------------------------------------
// Sesja: przydzial adresu, reset magistrali, dolaczanie
// ---------------------------------------------------------------------------
void clearPendingTx()
{
    s_txQueue.clear();
    UnilinkBus::clearGrant();
    s_slotKind          = SlotKind::None;
    s_slotCarriesScreen = false;
    s_slotIsStatusC0    = false;
    s_breakAwaitingPoll = false;
}

void adoptAddress(uint8_t addr, uint8_t appointCmd2, unsigned long now, const char* how)
{
    const bool changed = !s_appointed || s_myAddr != addr;
    s_appointed        = true;
    s_myAddr           = addr;
    s_nvsAddr          = 0;
    s_dualListenWaited = false;
    s_magicStreak      = 0;
    s_appointMs        = now;
    s_lastOwnPingMs    = now;
    s_displayYielded   = false;
    if ((appointCmd2 & 0x0F) != 0) s_claimMask = (uint8_t)(appointCmd2 & 0x0F);
    if (changed) {
        clearPendingTx();
        s_shown.valid = false;
        resetCdTextCache();
        CdChanger::resetToAllocated();
    }
    s_persistAddrPending = true;
    Serial.printf(">> Adres 0x%02X (%s), maska 0x%02X\n", addr, how, s_claimMask);
}

void onBusReset(unsigned long now)
{
    const bool wasAppointed = s_appointed;
    s_appointed        = false;
    s_myAddr           = ADDR_GROUP_CD;
    s_nvsAddr          = 0;
    s_dualListenWaited = false;
    s_displayYielded   = false;
    clearPendingTx();
    s_shown.valid = false;
    resetCdTextCache();
    Diagnostics::recordNote("RESET");
    if (wasAppointed) {
        Serial.println(">> Radio: reset magistrali (18 10 01 00) — ponowne discovery");
        if (millis() > CRASHLOG_GRACE_MS && (now - s_busOnSinceMs) > CRASHLOG_BUS_ON_GRACE_MS) {
            Serial.captureCrashSnapshot("RADIO BUS RESET 18 10 01 00");
        }
    } else {
        Serial.println(">> Radio: reset magistrali — discovery");
    }
}

void maybeSendMagic(const Frame& f, unsigned long now)
{
    if (s_nvsAddr != 0 && !s_dualListenWaited) {
        s_dualListenWaited = true;
        return;
    }
    if (now - s_magicSentMs < MAGIC_MIN_INTERVAL_MS) return;
    if (s_magicStreak >= MAGIC_MAX_ATTEMPTS && now - s_magicSentMs < MAGIC_BACKOFF_MS) return;
    if (s_magicStreak >= MAGIC_MAX_ATTEMPTS) s_magicStreak = 0;

    static const uint8_t magic[6] = { 0x10, 0x18, 0x04, 0x00, 0x2C, 0x00 };
    if (UnilinkBus::respond(f, magic, sizeof(magic))) {
        s_magicSentMs = now;
        s_magicStreak++;
        s_nvsAddr = 0;
        Diagnostics::recordFrame("TX", magic, sizeof(magic));
        Serial.printf(">> 01 11 bez przydzialu: zglaszam sie (10 18 04 00), proba %u\n", s_magicStreak);
    }
}

void stopByRadio(const char* why)
{
    const MechState ms = CdChanger::mechState();
    if (ms == MechState::Init || ms == MechState::Idle) return;
    Serial.printf(">> Radio: stop odtwarzania (%s)\n", why);
    CdChanger::sleep();
    clearPendingTx();
}

// ---------------------------------------------------------------------------
// Komendy od radia
// ---------------------------------------------------------------------------
void handleRequest84(const Frame& f)
{
    const uint8_t c2 = f.data[3];
    if (c2 == 0xD9) {
        enqueueTextField8(false, f.data[5]);
    } else if (c2 == 0xD7) {
        const uint8_t d4 = (f.len >= 11) ? f.data[8] : 0x01;
        if (d4 == 0x00) {
            enqueueDiscTextBlock(discFromRequest(f));
        } else {
            enqueueTrackTextBlock(true, true);
        }
    } else if (c2 == 0xD2) {
        enqueueTrackTextBlock(true, true);
    } else if (c2 == 0xDA || c2 == 0xDD) {
        enqueueDiscTextBlock(discFromRequest(f));
    } else if (c2 == 0x95) {
        enqueueMagazineMap();
    } else if (c2 == 0x97) {
        enqueueDiscInfo(discFromRequest(f));
    }
}

void handleDirectSelect(const Frame& f)
{
    const uint8_t disc = (uint8_t)(f.data[3] & 0x0F);
    uint8_t track = 1;
    if (f.len >= 11) {
        const uint8_t raw = f.data[5];
        if (raw == 0xFF || raw == 0x00) track = 1;
        else if ((raw & 0xF0) == 0xF0) track = (uint8_t)(raw & 0x0F);
        else track = UnilinkFrame::decodeBcd(raw);
        if (track == 0) track = 1;
    }
    const bool discValid = disc >= 1 && disc <= MAX_DISC &&
                           ((Magazine::presenceMap() >> (disc - 1)) & 1);
    uint8_t maxTrack = audioGetTrackCount(disc);
    if (maxTrack == 0) maxTrack = MAX_TRACKS;
    if (discValid && track >= 1 && track <= maxTrack) {
        CdChanger::selectDiscTrack(disc, track);
        onDiscChanged(disc);
        serviceCdText(millis());
        Serial.printf(">> 0xB0: CD%u TR%u\n", disc, track);
    } else {
        Serial.printf(">> 0xB0 odrzucone: CD%u TR%u (max %u)\n", disc, track, maxTrack);
    }
}

void handleCommand(const Frame& f, unsigned long now)
{
    s_selected = true;
    if (s_displayYielded) {
        s_displayYielded = false;
        s_shown.valid    = false;
    }
    const uint8_t c1 = f.data[2];
    switch (c1) {
        case 0x20:
            CdChanger::handlePlayCommand();
            break;
        case 0x24: CdChanger::seek(+SEEK_STEP_SEC); break;
        case 0x25: CdChanger::seek(-SEEK_STEP_SEC); break;
        case 0x26: {
            const uint8_t oldDisc = CdChanger::disk();
            CdChanger::nextTrack();
            if (CdChanger::disk() != oldDisc) {
                onDiscChanged(CdChanger::disk());
            }
            serviceCdText(now);
            break;
        }
        case 0x27: {
            const uint8_t oldDisc = CdChanger::disk();
            CdChanger::prevTrack();
            if (CdChanger::disk() != oldDisc) {
                onDiscChanged(CdChanger::disk());
            }
            serviceCdText(now);
            break;
        }
        case 0x28:
            CdChanger::nextDisc();
            onDiscChanged(CdChanger::disk());
            serviceCdText(now);
            break;
        case 0x29:
            CdChanger::prevDisc();
            onDiscChanged(CdChanger::disk());
            serviceCdText(now);
            break;
        case 0x34: CdChanger::toggleRepeat();  break;
        case 0x35: CdChanger::toggleShuffle(); break;
        case 0x36: CdChanger::toggleIntro();   break;
        case 0x84: handleRequest84(f); break;
        case 0xB0: handleDirectSelect(f); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Ramki z magistrali
// ---------------------------------------------------------------------------
uint8_t pingAddrNow()
{
    if (s_appointed) return s_myAddr;
    return s_nvsAddr;
}

void handleFrame(const Frame& f, unsigned long now)
{
    if (UnilinkFrame::validate(f.data, f.len) != UnilinkFrame::ValidateResult::Ok) {
        Diagnostics::recordFrame("BAD", f.data, f.len);
        s_stat.bad++;
        return;
    }
    s_lastFrameMs    = now;
    s_silentReported = false;
    Diagnostics::recordFrame("RX", f.data, f.len);
    if (f.replyLen) Diagnostics::recordFrame("TX", f.reply, f.replyLen);

    const uint8_t rad = f.data[0];
    const uint8_t tad = f.data[1];
    const uint8_t c1  = f.data[2];
    const uint8_t c2  = f.data[3];
    const bool fromMaster = (tad == ADDR_MASTER);

    // Reset magistrali obowiazuje zawsze, takze przy BUS_ON=0.
    if (rad == ADDR_BROADCAST && fromMaster && c1 == 0x01 && c2 == 0x00) {
        onBusReset(now);
        return;
    }
    if (!s_busActive) return;

    // --- Ping 01 12 (odpowiada przerwanie) ---
    if (c1 == 0x01 && c2 == 0x12 && (tad == ADDR_MASTER || tad == ADDR_DISPLAY)) {
        const uint8_t pa = pingAddrNow();
        if (pa == 0 || rad != pa) return;
        if (!f.replyLen) {
            uint8_t r[6] = { tad, rad, 0x00, UnilinkFrame::statusByte(CdChanger::mechState()), 0, 0 };
            r[4] = UnilinkFrame::parity1(r[0], r[1], r[2], r[3]);
            if (UnilinkBus::respond(f, r, sizeof(r))) Diagnostics::recordFrame("TX", r, sizeof(r));
        }
        s_stat.pings++;
        s_lastOwnPingMs = now;
        if (!s_appointed && rad == s_nvsAddr) {
            adoptAddress(rad, 0x00, now, "radio pamieta adres z poprzedniej sesji");
        }
        if (fromMaster) CdChanger::noteFirstPing();
        CdChanger::notePolled();
        return;
    }

    // --- Request polling 18 10 01 15 (zglasza nas przerwanie) ---
    if (rad == ADDR_BROADCAST && fromMaster && c1 == 0x01 && c2 == 0x15) {
        s_stat.poll15++;
        s_lastPoll15Ms      = now;
        s_breakAwaitingPoll = false;
        s_breakFailStreak   = 0;
        s_noWaveStreak      = 0;
        s_nextBreakMs       = 0;
        if (f.replyLen) s_stat.claims++;
        return;
    }

    // --- Grant 3X 10 01 13 (ramke wysyla przerwanie) ---
    if (s_appointed && rad == s_myAddr && fromMaster && c1 == 0x01 && c2 == 0x13) {
        if (!s_selected) {
            s_selected       = true;
            s_displayYielded = false;
            s_shown.valid    = false;
        }
        if (!f.replyLen) {
            uint8_t fr[16];
            DisplayKey key;
            const int n = buildDisplayFrame(fr, key);
            if (UnilinkBus::respond(f, fr, n)) {
                Diagnostics::recordFrame("TX", fr, n);
                s_shown = key;
                if (s_shown.seconds != 0xFF) s_lastTimeFrameMs = now;
            }
        }
        s_stat.grants++;
        CdChanger::notePolled();
        return;
    }

    // --- ANYONE? 18 10 01 02 ---
    if (rad == ADDR_BROADCAST && fromMaster && c1 == 0x01 && c2 == 0x02) {
        // Jezeli master rozpoczyna re-enumeracje magistrali (>2 s od ostatniego appoint),
        // resetujemy sesje i bierzemy udzial w discovery.
        if (s_appointed && (now - s_appointMs) > 2000) {
            s_nvsAddr   = s_myAddr;
            s_appointed = false;
            s_selected  = false;
        }
        if (s_appointed) return;
        uint8_t info[11];
        buildDeviceInfo(ADDR_GROUP_CD, info);
        if (UnilinkBus::respond(f, info, sizeof(info))) {
            s_anyoneAnsweredMs = now;
            Diagnostics::recordFrame("TX", info, sizeof(info));
            Serial.println(">> ANYONE?: zglaszam zmieniarke (10 30 8C)");
        }
        return;
    }

    // --- Appoint 3X 10 02 YY ---
    if (fromMaster && c1 == 0x02 && (rad & 0xF0) == ADDR_GROUP_CD) {
        const bool forUs = (!s_appointed && (now - s_anyoneAnsweredMs) < APPOINT_AFTER_ANYONE_MS) ||
                           (!s_appointed && s_nvsAddr != 0 && rad == s_nvsAddr) ||
                           (s_appointed && rad == s_myAddr);
        if (!forUs) return;
        uint8_t info[11];
        buildDeviceInfo(rad, info);
        if (UnilinkBus::respond(f, info, sizeof(info))) {
            Diagnostics::recordFrame("TX", info, sizeof(info));
        }
        adoptAddress(rad, c2, now, "appoint");
        return;
    }

    // --- 18 10 01 11: czy jest nieprzydzielona zmieniarka? ---
    if (rad == ADDR_BROADCAST && fromMaster && c1 == 0x01 && c2 == 0x11) {
        if (!s_appointed) maybeSendMagic(f, now);
        return;
    }

    if (!s_appointed) return;

    // --- Arbitraz ekranu 0x70 (ochrona przed walka z Masterem/Tunerem z crash_00001.txt) ---
    if (rad == 0x70) {
        if (tad != s_myAddr && tad != ADDR_MASTER && tad != ADDR_DISPLAY &&
            (c1 == 0xC0 || c1 == 0x90 || c1 == 0x92)) {
            // Inne zrodlo (np. Tuner 0x51 przy komunikacie TA/RDS) pisze na ekran 0x70:
            // oddajemy ekran do czasu ponownego wyboru zmieniarki.
            if (!s_displayYielded) {
                s_displayYielded = true;
                clearPendingTx();
                Serial.printf(">> Ekran 0x70 przejety przez 0x%02X (cmd 0x%02X) — wstrzymuje ramki 0x70\n", tad, c1);
            }
            return;
        }
        if (fromMaster && c1 == 0x25 && c2 == 0x00) {
            if (!s_displayYielded) {
                s_displayYielded = true;
                clearPendingTx();
                Serial.println(">> Master: 70 10 25 00 — wstrzymuje ramki ekranu 0x70");
            }
            return;
        }
        if (fromMaster && c1 == 0xC0 && c2 == 0x00 &&
            s_shown.valid && s_shown.status == 0x00 &&
            s_lastTimeFrameMs != 0 && (now - s_lastTimeFrameMs) < 120) {
            // Master nadpisal nasz swiezo wyslany tick 0x90 pusta ramka 70 10 C0 00:
            // nie walczymy o ekran, zeby nie wywolac resetu magistrali 18 10 01 00!
            if (!s_displayYielded) {
                s_displayYielded = true;
                clearPendingTx();
                Serial.println(">> Master nadpisal ekran 0x70 po naszym 0x90 — ustępuje ekranu");
            }
            return;
        }
        if (fromMaster && c1 == 0x91 && f.len >= 6 && f.data[5] == s_myAddr) {
            // Master przelaczyl widok ekranu 0x70 z powrotem na nasze zrodlo (70 10 91 .. 31)
            if (s_displayYielded) {
                s_displayYielded = false;
                s_shown.valid    = false;
                Serial.println(">> Master przywrocil ekran 0x70 dla zmieniarki");
            }
            return;
        }
    }

    // --- Rozglaszane informacje radia ---
    if (rad == ADDR_BROADCAST) {
        if (fromMaster && c1 == 0x08 && c2 == 0x00) {
            if (CdChanger::isSeeking()) {
                s_justEndedSeek = true;
            }
            CdChanger::stopSeekScan();   // puszczenie klawisza FF/REW
        } else if (c1 == 0x87 && (tad == ADDR_MASTER || tad == 0x11)) {
            if ((c2 & 0x01) == 0) {
                // Ramka 87 z bitem 0=0 to soft-mute/wyciszenie audio w przedwzmacniaczu radia (np. przy starcie radia 87 2A).
                // Zgodnie ze specyfikacja Sony i logami OE zmieniarka nie zatrzymuje wtedy mechanizmu i nie zeruje s_selected!
                Serial.printf(">> [Audio Bus] Radio sygnalizuje audio MUTE (87 %02X)\n", c2);
            } else {
                const bool wasSelected = s_selected;
                s_selected       = true;
                s_displayYielded = false;
                if (!wasSelected) s_shown.valid = false;
                // Jezeli radio wlaczylo tor audio zmieniarki (87 29), a my stoimy w Init/Idle
                // (np. radio nie przyslalo osobnego 0x20 PLAY po starcie), ruszamy od razu!
                const MechState ms = CdChanger::mechState();
                if (ms == MechState::Init || ms == MechState::Idle) {
                    Serial.println(">> [0x87] Audio ON dla zmieniarki -> start odtwarzania!");
                    CdChanger::handlePlayCommand();
                }
            }
        } else if (fromMaster && c1 == 0xF0) {
            if (c2 == s_myAddr) {
                const bool wasSelected = s_selected;
                s_selected       = true;
                s_displayYielded = false;
                if (!wasSelected) s_shown.valid = false;
                const MechState ms = CdChanger::mechState();
                if (ms == MechState::Init || ms == MechState::Idle) {
                    Serial.println(">> [0xF0] Radio wybralo zmieniarke -> start odtwarzania!");
                    CdChanger::handlePlayCommand();
                }
            } else {
                s_selected = false;
                stopByRadio("radio wybralo inne zrodlo");
            }
        }
        return;
    }

    if (rad == s_myAddr) handleCommand(f, now);
}

// ---------------------------------------------------------------------------
// Planowanie nadawania
// ---------------------------------------------------------------------------
void loadDisplaySlot(unsigned long now)
{
    (void)now;
    uint8_t f[16];
    DisplayKey key;
    const int n = buildDisplayFrame(f, key);
    s_slotClaimBase = UnilinkBus::claimsSent();
    UnilinkBus::loadGrant(f, n, s_claimMask);
    s_slotKind          = SlotKind::Display;
    s_slotKey           = key;
    s_slotCarriesScreen = true;
    s_slotIsStatusC0    = (f[2] == 0xC0 || f[2] == 0x8E);
}

void loadNextSlot(unsigned long now)
{
    const bool cdTextAllowed = (CdChanger::mechState() == MechState::Playing) &&
                               !CdChanger::isSeeking() && !s_displayYielded &&
                               s_shown.valid && (s_shown.status == 0x00) && (s_shown.seconds != 0xFF);

    // 1. Pilne ramki protokolowe (status 0x9C, DiscID 0xC5/0xD5, magazynek 0x95/0x97)
    //    maja pierwszenstwo przed wszystkim.
    if (!s_txQueue.isEmpty()) {
        const int prio = s_txQueue.peekPriority();
        if (prio < Tx::PRIO_CD_TEXT) {
            Tx::TxItem item;
            if (s_txQueue.dequeue(item)) {
                if (item.len == 16 && item.bytes[2] == 0xC0) {
                    const DisplayKey qk = keyFromC0(item.bytes);
                    const DisplayKey nk = keyNow();
                    if (qk.disc != nk.disc || qk.track != nk.track || qk.status != nk.status) {
                        loadNextSlot(now);
                        return;
                    }
                }
                s_slotClaimBase = UnilinkBus::claimsSent();
                UnilinkBus::loadGrant(item.bytes, item.len, s_claimMask);
                s_slotKind          = SlotKind::Queued;
                s_slotCarriesScreen = (item.len == 16 && item.bytes[2] == 0xC0);
                s_slotIsStatusC0    = s_slotCarriesScreen;
                if (s_slotCarriesScreen) s_slotKey = keyFromC0(item.bytes);
                return;
            }
        }
    }

    // 2. Ramka ekranu (0xC0 lub 1 Hz tick 0x90) ma pierwszenstwo przed CD-TEXTem,
    //    dzieki czemu licznik 1 Hz nigdy nie potyka sie w trakcie wysylania nazw!
    if (needDisplayFrame(now)) {
        loadDisplaySlot(now);
        return;
    }

    // 3. W oknie miedzy sekundami wysylamy oczekujace segmenty CD-TEXT (D2/DA/D7).
    if (!s_txQueue.isEmpty() && cdTextAllowed) {
        Tx::TxItem item;
        if (s_txQueue.dequeue(item)) {
            s_slotClaimBase     = UnilinkBus::claimsSent();
            UnilinkBus::loadGrant(item.bytes, item.len, s_claimMask);
            s_slotKind          = SlotKind::Queued;
            s_slotCarriesScreen = false;
            s_slotIsStatusC0    = false;
            return;
        }
    }

    if (s_slotKind != SlotKind::None) {
        UnilinkBus::clearGrant();
        s_slotKind          = SlotKind::None;
        s_slotCarriesScreen = false;
        s_slotIsStatusC0    = false;
    }
}

bool displaySlotStale()
{
    const DisplayKey k = keyNow();
    if (k.disc != s_slotKey.disc || k.track != s_slotKey.track || k.status != s_slotKey.status) return true;
    if (k.status == 0x00 && s_slotKey.seconds == 0xFF && k.seconds != 0xFF) return true;
    if (s_slotKey.seconds == 0xFF) return false;
    return k.minutes != s_slotKey.minutes || k.seconds != s_slotKey.seconds || k.flags != s_slotKey.flags;
}

void trackGrants(unsigned long now)
{
    const uint32_t claims = UnilinkBus::claimsSent();
    if (claims != s_claimsSeen) {
        s_claimsSeen  = claims;
        s_lastClaimMs = now;
    }
    const uint32_t served = UnilinkBus::grantsServed();
    if (served != s_grantsSeen) {
        s_grantsSeen = served;
        if (s_slotKind != SlotKind::None && s_slotCarriesScreen) {
            s_shown = s_slotKey;
            if (s_shown.seconds != 0xFF) s_lastTimeFrameMs = now;
            s_justEndedSeek = false;
        }
        s_slotKind          = SlotKind::None;
        s_slotCarriesScreen = false;
        s_slotIsStatusC0    = false;
    }
}

void maybeBreak(unsigned long now)
{
    const MechState ms = CdChanger::mechState();
    if (ms == MechState::Init || !s_selected || !s_busActive || !s_appointed) {
        s_breakAwaitingPoll = false;
        return;
    }
    // Master cyklicznie rozglasza 18 10 01 15 (tak jak w oryginalnym radiu OE).
    // Gdy master aktywnie odpytuje 01 15, NIE wolno zwierac linii DATA do masy przez Slave Break,
    // bo niszczy to ramki na magistrali i powoduje wywalenie radia (18 10 01 00 Reset).
    if (s_lastPoll15Ms != 0 && (now - s_lastPoll15Ms) < BREAK_MASTER_POLL_TIMEOUT_MS) {
        s_breakAwaitingPoll = false;
        return;
    }
    if (s_breakAwaitingPoll) {
        if (now - s_breakDoneMs < BREAK_POLL_WAIT_MS) return;
        s_breakAwaitingPoll = false;
        s_stat.breakNoPoll++;
        if (s_breakFailStreak < 6) s_breakFailStreak++;
        unsigned long backoff = BREAK_RETRY_FAIL_MS << (s_breakFailStreak - 1);
        if (backoff > BREAK_BACKOFF_MAX_MS) backoff = BREAK_BACKOFF_MAX_MS;
        s_nextBreakMs = now + backoff;
        return;
    }
    if (s_nextBreakMs != 0 && (long)(now - s_nextBreakMs) < 0) return;
    if (UnilinkBus::isTransmitting()) return;

    const UnilinkBus::BreakResult r = UnilinkBus::trySlaveBreak();
    const unsigned long after = millis();
    if (r == UnilinkBus::BreakResult::Done) {
        s_breakAwaitingPoll = true;
        s_breakDoneMs       = after;
        s_noWaveStreak      = 0;
        s_stat.breaks++;
        Diagnostics::recordNote("BREAK");
    } else if (r == UnilinkBus::BreakResult::Busy) {
        s_nextBreakMs = after + BREAK_RETRY_BUSY_MS;
    } else {
        s_stat.breakNoWave++;
        if (s_noWaveStreak < 6) s_noWaveStreak++;
        unsigned long backoff = BREAK_RETRY_NOWAVE_MS << (s_noWaveStreak - 1);
        if (backoff > BREAK_BACKOFF_MAX_MS) backoff = BREAK_BACKOFF_MAX_MS;
        s_nextBreakMs = after + backoff;
    }
}

void scheduleTx(unsigned long now)
{
    if (!s_appointed || !s_busActive) {
        if (s_slotKind != SlotKind::None) {
            UnilinkBus::clearGrant();
            s_slotKind          = SlotKind::None;
            s_slotCarriesScreen = false;
            s_slotIsStatusC0    = false;
        }
        s_breakAwaitingPoll = false;
        return;
    }

    // Nie ruszamy slotu ani nie wywolujemy Slave Break, dopoki przerwanie
    // aktualnie taktuje odpowiedz na magistrali.
    if (UnilinkBus::isTransmitting()) return;

    const MechState ms = CdChanger::mechState();
    if (ms == MechState::Init || !s_selected || s_displayYielded) {
        if (s_slotKind == SlotKind::Display) {
            UnilinkBus::clearGrant();
            s_slotKind          = SlotKind::None;
            s_slotCarriesScreen = false;
            s_slotIsStatusC0    = false;
        }
        s_breakAwaitingPoll = false;
        if (s_txQueue.isEmpty() || s_displayYielded) {
            return;
        }
    }

    // Jezeli przerwanie odrzucilo nadawanie grantu (abort) i przywrocilo bufor,
    // ale s_slotKind byl juz wyczyszczony lub odwrotnie, synchronizujemy stan.
    if (s_slotKind != SlotKind::None && !UnilinkBus::grantLoaded()) {
        s_slotKind          = SlotKind::None;
        s_slotCarriesScreen = false;
        s_slotIsStatusC0    = false;
    }

    const bool claimInFlight = (s_slotKind != SlotKind::None) &&
                               UnilinkBus::claimsSent() != s_slotClaimBase &&
                               (now - s_lastClaimMs) < CLAIM_GRANT_WAIT_MS;
    if (!claimInFlight) {
        if (s_slotKind == SlotKind::None) {
            loadNextSlot(now);
        } else if (s_slotKind == SlotKind::Display) {
            if (ms == MechState::Init || !s_selected || s_displayYielded) {
                UnilinkBus::clearGrant();
                s_slotKind          = SlotKind::None;
                s_slotCarriesScreen = false;
                s_slotIsStatusC0    = false;
            } else if (!s_txQueue.isEmpty() && s_txQueue.peekPriority() < Tx::PRIO_CD_TEXT) {
                // Pilne zadanie protokolowe (np. 84 95 / 84 97 / 9C / D5) zastepuje slot ekranu
                loadNextSlot(now);
            } else if (displaySlotStale()) {
                loadDisplaySlot(now);
            }
        }
    }
    if (s_slotKind != SlotKind::None && !claimInFlight) maybeBreak(now);
}

// ---------------------------------------------------------------------------
// BUS_ON, szybka sciezka, zdrowie sesji
// ---------------------------------------------------------------------------
void updateBusOn(unsigned long now)
{
    const bool raw = UnilinkBus::busOnRaw();
    if (raw != s_busRawLast) {
        s_busRawLast = raw;
        s_busEdgeMs  = now;
    }
    if (raw != s_busActive && (now - s_busEdgeMs) >= BUS_ON_DEBOUNCE_MS) {
        s_busActive = raw;
        if (raw) {
            s_busOffSinceMs  = 0;
            s_busOnSinceMs   = now;
            s_busOffStopped  = false;
            s_displayYielded = false;
            s_selected       = false; // Czekamy na F0 31, 87 ON lub komende do 0x31 (radio moze wlaczyc sie na FM Tuner)
            s_lastFrameMs    = now;
            s_lastOwnPingMs  = now;
            CdChanger::wake();
            Serial.println("=== BUS_ON = 1 ===");
        } else {
            s_busOffSinceMs = now;
            s_selected      = false;
            if (s_appointed) {
                s_nvsAddr   = s_myAddr;
                s_appointed = false;
            }
            UnilinkBus::abortTx();
            UnilinkBus::clearGrant();
            s_slotKind          = SlotKind::None;
            s_slotCarriesScreen = false;
            s_slotIsStatusC0    = false;
            s_breakAwaitingPoll = false;
            Serial.println("=== BUS_ON = 0 (sesja zachowana) ===");
        }
    }
    if (!s_busActive && !s_busOffStopped && s_busOffSinceMs != 0 &&
        now - s_busOffSinceMs >= BUS_OFF_AUDIO_STOP_MS) {
        s_busOffStopped = true;
        s_selected      = false;
        CdChanger::sleep();
        s_txQueue.clear();
        s_shown.valid = false;
        resetCdTextCache();
        Serial.println("=== Radio wylaczone: stop audio ===");
    }
}

void updateFastPath()
{
    static bool    lastEnabled = false;
    static uint8_t lastPing = 0, lastStatus = 0xFF, lastMe = 0;
    const bool    enabled = s_busActive;
    const uint8_t ping    = pingAddrNow();
    const uint8_t status  = UnilinkFrame::statusByte(CdChanger::mechState());
    const uint8_t me      = s_appointed ? (uint8_t)s_myAddr : 0;
    if (enabled != lastEnabled || ping != lastPing || status != lastStatus || me != lastMe) {
        lastEnabled = enabled;
        lastPing    = ping;
        lastStatus  = status;
        lastMe      = me;
        UnilinkBus::setFastPath(enabled, ping, status, me);
    }
}

void serviceHealth(unsigned long now)
{
    if (!s_busActive || !s_selected) return;
    if (!s_silentReported && s_lastFrameMs != 0 && now - s_lastFrameMs > RADIO_SILENT_MS) {
        s_silentReported = true;
        Serial.println(">> Radio milczy (BUS_ON=1, brak ramek)");
        if (millis() > CRASHLOG_GRACE_MS) Serial.captureCrashSnapshot("RADIO SILENT (BUS_ON=1)");
    }
    if (s_appointed && now - s_lastOwnPingMs > RADIO_FORGOT_US_MS && now - s_lastFrameMs < 1000) {
        Serial.printf(">> Radio nie pinguje 0x%02X od %lus — ponowne dolaczenie\n",
                      (uint8_t)s_myAddr, (now - s_lastOwnPingMs) / 1000);
        const uint8_t old = s_myAddr;
        s_appointed        = false;
        s_myAddr           = ADDR_GROUP_CD;
        s_nvsAddr          = old;
        s_dualListenWaited = true;
        clearPendingTx();
        s_shown.valid = false;
        resetCdTextCache();
        if (millis() > CRASHLOG_GRACE_MS) Serial.captureCrashSnapshot("RADIO STOPPED PINGING US");
    }
}

void serviceStats(unsigned long now)
{
    if (now - s_lastStatMs < 2000) return;
    s_lastStatMs = now;
    UnilinkBus::Stats bs;
    UnilinkBus::takeStats(bs);
    if (s_busActive || bs.frames) {
        Serial.printf("[STAT] rx=%lu ping=%u p15=%u claim=%u grant=%u brk=%u/%u/%u late=%lu abort=%lu bad=%u glitch=%lu cut=%lu ovf=%lu | %s0x%02X st=0x%02X CD%u TR%u %02u:%02u q=%u\n",
                      (unsigned long)bs.frames, s_stat.pings, s_stat.poll15, s_stat.claims, s_stat.grants,
                      s_stat.breaks, s_stat.breakNoPoll, s_stat.breakNoWave,
                      (unsigned long)bs.txLate, (unsigned long)bs.txAborted, s_stat.bad,
                      (unsigned long)bs.glitches, (unsigned long)bs.broken, (unsigned long)bs.overflow,
                      s_appointed ? "" : "!", s_appointed ? (uint8_t)s_myAddr : s_nvsAddr,
                      UnilinkFrame::statusByte(CdChanger::mechState()),
                      CdChanger::disk(), CdChanger::track(), CdChanger::minutes(), CdChanger::seconds(),
                      (unsigned)s_txQueue.size());
    }
    s_stat = ProtoStats{};
}

void busTask(void*)
{
    if (!UnilinkBus::begin(xTaskGetCurrentTaskHandle())) {
        Serial.println("[Unilink] BLAD: nie udalo sie uruchomic przerwania zegara!");
    } else {
        Serial.printf("[Unilink] Magistrala gotowa, przerwanie zegara %s\n",
                      UnilinkBus::isrInIram() ? "w IRAM" : "POZA IRAM (zapis flash zaklocza odbior!)");
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, 1);
        updateBusOn(millis());
        UnilinkBus::service();

        Frame f;
        for (int i = 0; i < 16 && UnilinkBus::popFrame(f); ++i) {
            handleFrame(f, millis());
        }
        unsigned long now = millis();
        trackGrants(now);

        CdChanger::serviceMediaMount();
        CdChanger::update(now, s_appointed, s_selected);
        const uint8_t dBeforeAuto = CdChanger::disk();
        CdChanger::serviceAutoAdvance();
        if (CdChanger::disk() != dBeforeAuto) {
            onDiscChanged(CdChanger::disk());
        }
        CdChanger::serviceSeekRepeat(now);
        serviceCdText(now);

        updateFastPath();
        scheduleTx(millis());
        now = millis();
        serviceHealth(now);
        serviceStats(now);
    }
}

void writeAddress()
{
    const uint8_t addr = s_myAddr;
    const uint8_t mask = s_claimMask;
    if (!s_appointed) return;
    if (s_savedAlloc && addr == s_savedAddr && mask == s_savedMask) return;
    s_prefs.putUChar("myAddr", addr);
    s_prefs.putBool("allocated", true);
    s_prefs.putUChar("claimMask", mask);
    s_savedAddr  = addr;
    s_savedMask  = mask;
    s_savedAlloc = true;
    Serial.printf("[NVS] Zapisano adres 0x%02X (maska 0x%02X)\n", addr, mask);
}

} // namespace

// =============================================================================
// API
// =============================================================================
void begin()
{
    s_prefs.begin(PREFS_NAMESPACE, false);
    s_savedAddr  = s_prefs.getUChar("myAddr", ADDR_GROUP_CD);
    s_savedAlloc = s_prefs.getBool("allocated", false);
    s_savedMask  = s_prefs.getUChar("claimMask", CLAIM_MASK_DEFAULT);
    if (s_savedMask != 0 && s_savedMask <= 0x0F) s_claimMask = s_savedMask;
    if (s_savedAlloc && (s_savedAddr & 0xF0) == ADDR_GROUP_CD && s_savedAddr != ADDR_GROUP_CD) {
        s_nvsAddr = s_savedAddr;
        Serial.printf("[Unilink] Adres z poprzedniej sesji: 0x%02X (odpowiadam na ping, czekam na discovery)\n", s_nvsAddr);
    }
    xTaskCreatePinnedToCore(busTask, "unilink", UNILINK_TASK_STACK, nullptr,
                            UNILINK_TASK_PRIORITY, &s_task, UNILINK_TASK_CORE);
}

bool isAllocated() { return s_appointed; }
bool isSelected()  { return s_selected; }
bool busActive()   { return s_busActive; }

void enqueue(uint8_t priority, const uint8_t* bytes, int len)
{
    enqueueFrame(priority, bytes, len);
}

// 0x94 — ikony trybow (Repeat/Shuffle/Intro).
void enqueueModeIconsHelper(uint8_t repeatMode, bool shuffle, bool intro)
{
    uint8_t d[UnilinkFrame::ICON_DATA_LEN];
    UnilinkFrame::encodeIconData(repeatMode, shuffle, intro, d);
    enqueueMiddle(0x70, 0x94, 0x00, d, Tx::PRIO_MAGAZINE);
}

bool flashWriteWindow()
{
    if (!s_busActive) return true;
    if (!s_appointed) return false;
    const unsigned long now = millis();
    if (now - s_appointMs < FLASH_AFTER_APPOINT_MS) return false;
    if (UnilinkBus::isTransmitting()) return false;
    const unsigned long sincePing = now - s_lastOwnPingMs;
    return sincePing >= FLASH_WINDOW_AFTER_PING_MIN_MS && sincePing <= FLASH_WINDOW_AFTER_PING_MAX_MS;
}

void servicePersist(bool flashWriteAllowed)
{
    if (!flashWriteAllowed || !s_persistAddrPending) return;
    s_persistAddrPending = false;
    writeAddress();
}

void persistNow()
{
    if (!s_persistAddrPending) return;
    s_persistAddrPending = false;
    writeAddress();
}

} // namespace UnilinkProtocol
