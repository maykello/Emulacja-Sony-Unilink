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
bool          s_selected         = true;   // radio wybralo nas jako zrodlo

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
    bool    valid;
};
// Co radio faktycznie ma na ekranie (ostatnia DOSTARCZONA ramka ekranu).
DisplayKey s_shown = { 0, 0, 0xFF, 0xFF, 0xFF, false };

enum class SlotKind : uint8_t { None, Queued, Display };
SlotKind      s_slotKind     = SlotKind::None;
DisplayKey    s_slotKey      = { 0, 0, 0xFF, 0xFF, 0xFF, false };
bool          s_slotCarriesScreen = false;   // dostarczenie aktualizuje s_shown
uint32_t      s_slotClaimBase = 0;           // claimsSent() w chwili zaladowania slotu
uint32_t      s_grantsSeen   = 0;
uint32_t      s_claimsSeen   = 0;
unsigned long s_lastClaimMs  = 0;
unsigned long s_lastTimeFrameMs = 0;

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
// prawdziwej zmieniarki (CD8): 0x88 tekst jeszcze nie poszedl, 0x8B nazwy wlasnie
// wyslane, 0x8A stan ustalony — dopiero wtedy procesor ekranu (0x71) prosi o
// nazwy ramka 84 D7.
// ---------------------------------------------------------------------------
uint8_t s_textFlagState = 0;   // 0 = brak nazw, 1 = swiezo wyslane, 2 = ustalone

uint8_t discFlagsNibble()
{
    if (audioGetTrackCount(CdChanger::disk()) == 0 && !Diagnostics::hasError()) return 0x00;
    if (CdChanger::isSeeking()) return 0x08;
    uint8_t flags = 0x08;
    if (s_textFlagState >= 1) flags |= 0x02;
    if (s_textFlagState == 1) flags |= 0x01;
    return flags;
}

inline void settleTextFlag()
{
    if (s_textFlagState == 1) s_textFlagState = 2;
}

uint8_t decodeFpad(uint8_t v)
{
    return ((v & 0xF0) == 0xF0) ? (uint8_t)(v & 0x0F) : UnilinkFrame::decodeBcd(v);
}

DisplayKey keyNow()
{
    DisplayKey k;
    k.disc    = CdChanger::disk();
    k.track   = CdChanger::track();
    k.status  = UnilinkFrame::statusByte(CdChanger::mechState());
    k.minutes = CdChanger::minutes();
    k.seconds = CdChanger::seconds();
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
    k.valid   = true;
    return k;
}

// ---------------------------------------------------------------------------
// Ramki ekranu
// ---------------------------------------------------------------------------
// 0xC0 — pelny status, 1:1 ze sniffu CDX-M670:
//   70 31 C0 00 61 00 00 00 00 30 F1 F0 00 88 FA 00   (gra, TR1 0:00, CD8)
//   77 31 C0 20 88 00 00 00 00 30 F2 FF FF 88 30 00   (zmiana utworu, czas nieznany)
void buildStatusC0(uint8_t* f)
{
    const MechState ms   = CdChanger::mechState();
    const uint8_t   disc = CdChanger::disk();
    const bool timeKnown = (ms == MechState::Playing || ms == MechState::Seeking);
    const bool discPresent = (audioGetTrackCount(disc) > 0) || Diagnostics::hasError();

    f[0]  = (ms == MechState::ChangedCd) ? 0x77 : 0x70;
    f[1]  = s_myAddr;
    f[2]  = 0xC0;
    f[3]  = UnilinkFrame::statusByte(ms);
    f[4]  = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    f[5]  = 0x00;
    f[6]  = 0x00;
    f[7]  = 0x00;
    f[8]  = 0x00;
    f[9]  = discPresent ? 0x30 : 0x00;
    f[10] = UnilinkFrame::encodeBcdFpad(CdChanger::track());
    f[11] = timeKnown ? UnilinkFrame::encodeBcdFpad(CdChanger::minutes()) : 0xFF;
    f[12] = timeKnown ? UnilinkFrame::encodeBcd(CdChanger::seconds()) : 0xFF;
    f[13] = UnilinkFrame::discHighNibble(disc, discFlagsNibble());
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

// 0x8E — ekran zmieniarki w spoczynku: 70 31 8E C0 EF 00 00 00 80 6F 00
void buildIdle8E(uint8_t* f)
{
    f[0]  = 0x70;
    f[1]  = s_myAddr;
    f[2]  = 0x8E;
    f[3]  = 0xC0;
    f[4]  = UnilinkFrame::parity1(f[0], f[1], f[2], f[3]);
    f[5]  = 0x00;
    f[6]  = 0x00;
    f[7]  = 0x00;
    f[8]  = UnilinkFrame::discHighNibble(CdChanger::disk(), 0x00);
    f[9]  = UnilinkFrame::parity2(f[4], &f[5], 4);
    f[10] = 0x00;
}

bool screenChanged(const DisplayKey& k)
{
    return !s_shown.valid || k.disc != s_shown.disc || k.track != s_shown.track ||
           k.status != s_shown.status;
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
    // Pelny 0xC0 przy zmianie plyty/utworu/stanu i w stanach przejsciowych.
    // FF/REW: tylko 0x90 (czas plynie, ekran sie nie przeladowuje).
    // 0xC0 w Playing przeladowuje ekran i zmazuje CD-TEXT.
    if (screenChanged(key) ||
        ms == MechState::LoadingTrack || ms == MechState::ChangedCd) {
        buildStatusC0(f);
        key = keyFromC0(f);
        return 16;
    }
    buildTimeTick90(f);
    return 11;
}

bool needDisplayFrame(unsigned long now)
{
    const MechState ms = CdChanger::mechState();
    if (ms == MechState::Init) return false;
    const DisplayKey k = keyNow();
    if (screenChanged(k)) return true;
    if (ms == MechState::Playing || ms == MechState::Seeking) {
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

void enqueueStatusC0()
{
    uint8_t f[16];
    buildStatusC0(f);
    enqueueFrame(Tx::PRIO_STATUS, f, sizeof(f));
}

// ---------------------------------------------------------------------------
// CD-TEXT (wariant CDX-M670: 0xD2 utwor, 0xDA plyta, 0xD7 koniec bloku)
// ---------------------------------------------------------------------------
// Ze sniffu: 70 31 D2 4B BE 61 70 69 74 61 02 00 F1 10 D0 00  ("Kapita")
//            70 31 D2 6E E1 73 6B 69 65 20 74 01 F1 10 23 00  ("nskie t")
//            70 31 DA 00 7B 00 00 00 00 00 02 00 08 10 95 00  (pusta nazwa plyty)
//            70 31 D7 00 78 00 00 00 00 00 02 00 00 1C 96 00  (koniec bloku)
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

// Segment nazwy: do 6 znakow w slotach 0..5; nieostatni ma 0x02 w slocie 6,
// ostatni 0x00 w slocie 6 i 0x01 w slocie 7.
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

// Nazwa plyty w bloku utworu: jedna ramka 0xDA (do 6 znakow, marker konca).
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

// Blok nazwy utworu: D2 (segmenty) + DA (plyta) [+ D7 na zadanie radia].
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

// Tryb OBD: odswiezamy tylko nazwe "utworu" (dane telemetrii).
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

// Wariant 8-znakowy (zadanie 84 D9 z numerem pola w D1).
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

// Numer plyty z D1 zadania 0x84 (F|nr albo surowy); inaczej biezaca plyta.
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
}

void serviceCdText(unsigned long now)
{
    if (!s_appointed) return;
    const MechState ms = CdChanger::mechState();
    if (ms == MechState::Init || ms == MechState::Idle) return;
    // FF/REW: tylko plynacy czas. Ten sam utwor — nie wysylamy nazw od nowa.
    if (ms == MechState::Seeking) {
        s_wasSeeking = true;
        return;
    }
    if (s_wasSeeking) {
        s_wasSeeking = false;
    }

    // C0 20 / C0 00 przeladowuje ekran. Nazwy przed Playing znikaja w tej samej
    // chwili, w ktorej licznik rusza. Czekamy az radio dostanie Playing z czasem.
    if (ms != MechState::Playing) return;
    if (!s_shown.valid || s_shown.status != 0x00 || s_shown.seconds == 0xFF) return;
    if (s_shown.disc != CdChanger::disk() || s_shown.track != CdChanger::track()) return;

    const uint8_t disc   = CdChanger::disk();
    const uint8_t track  = CdChanger::track();
    const RepeatMode rep = CdChanger::repeatMode();
    const Diagnostics::SystemError err = Diagnostics::getError();

    const bool changed = (disc != s_textSentDisc || track != s_textSentTrack ||
                          rep != s_textSentRepeat || err != s_textSentError);
    bool obdRefresh = false;
    if (rep != RepeatMode::Off && now - s_lastObdMs >= OBD_UPDATE_INTERVAL_MS) {
        obdRefresh = true;
        s_lastObdMs = now;
    }
    if (!changed && !obdRefresh) return;

    if (changed) {
        s_textFlagState = 1;
        if (!enqueueTrackTextBlock(false, true)) return;
    } else {
        if (!enqueueTrackTextOnly()) return;
    }
    s_textSentDisc   = disc;
    s_textSentTrack  = track;
    s_textSentRepeat = rep;
    s_textSentError  = err;

    if (changed) {
        char name[64];
        char discName[64];
        trackTextNow(name, sizeof(name));
        discTextFor(disc, discName, sizeof(discName));
        Serial.printf(">> CD-TEXT: CD%u TR%u plyta=\"%s\" utwor=\"%s\"\n",
                      disc, track, discName, name);
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

// 10 31 C5 A2 A8 24 77 52 F0 ... 88 0D 00 (skan) / 10 31 D5 ... F1 ... (zmiana plyty)
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

// Powiadomienie o zmianie plyty. Sniff: 90 31 9C 00 5D 00 00 00 88 E5 00 (CD8
// w magazynku) i 90 31 9C 00 5D 00 00 00 10 6D 00 (pusty slot CD1).
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
    s_slotKind = SlotKind::None;
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
    const bool expected     = (now - s_magicSentMs) < 2000 ||
                              (s_busOnSinceMs != 0 && (now - s_busOnSinceMs) < CRASHLOG_BUS_ON_GRACE_MS);
    s_appointed        = false;
    s_myAddr           = ADDR_GROUP_CD;
    s_nvsAddr          = 0;
    s_dualListenWaited = false;
    clearPendingTx();
    s_shown.valid = false;
    resetCdTextCache();
    Diagnostics::recordNote("RESET");
    if (wasAppointed && !expected) {
        Serial.println(">> Radio: reset magistrali (18 10 01 00) — ponowne discovery");
        if (millis() > CRASHLOG_GRACE_MS) {
            Serial.captureCrashSnapshot("RADIO BUS RESET 18 10 01 00");
        }
    } else {
        Serial.println(">> Radio: reset magistrali — discovery");
    }
}

// Nieprzydzielona zmieniarka zglasza sie ramka 10 18 04 00 na 18 10 01 11 —
// radio robi wtedy reset magistrali i discovery, w ktorym nas dopisuje.
void maybeSendMagic(const Frame& f, unsigned long now)
{
    if (s_nvsAddr != 0 && !s_dualListenWaited) {
        // Najpierw dajemy radiu pelny cykl na ping pod adresem z poprzedniej sesji.
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
    s_txQueue.clear();
    if (s_slotKind != SlotKind::None && UnilinkBus::nowUs() - UnilinkBus::lastClaimUs() > 60000) {
        UnilinkBus::clearGrant();
        s_slotKind = SlotKind::None;
    }
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
        // D4 = 0 -> nazwa plyty, inaczej nazwa utworu (sniff: 31 71 84 D7 FD 70 00 00 01 6E 00).
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
    // 0xB0 to ramka middle: numer plyty w dolnym nibblu CMD2, utwor w D1
    // (0xFF/0x00 = sama plyta, od poczatku).
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
    const uint8_t c1 = f.data[2];
    switch (c1) {
        case 0x20:
            s_selected = true;
            CdChanger::handlePlayCommand();
            break;
        case 0x24: CdChanger::seek(+SEEK_STEP_SEC); break;
        case 0x25: CdChanger::seek(-SEEK_STEP_SEC); break;
        case 0x26: CdChanger::nextTrack(); serviceCdText(now); break;
        case 0x27: CdChanger::prevTrack(); serviceCdText(now); break;
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
    // Przy BUS_ON=0 radio rozmawia z wlasnymi urzadzeniami — nie odpowiadamy.
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
        s_breakAwaitingPoll = false;
        s_breakFailStreak   = 0;
        s_noWaveStreak      = 0;
        s_nextBreakMs       = 0;
        if (f.replyLen) s_stat.claims++;
        return;
    }

    // --- Grant 3X 10 01 13 (ramke wysyla przerwanie) ---
    if (s_appointed && rad == s_myAddr && fromMaster && c1 == 0x01 && c2 == 0x13) {
        if (!f.replyLen) {
            // Nic nie bylo przygotowane — prawdziwa zmieniarka nigdy nie zostawia
            // pustego slotu po grancie, wiec dajemy aktualny ekran.
            uint8_t fr[16];
            DisplayKey key;
            const int n = buildDisplayFrame(fr, key);
            if (UnilinkBus::respond(f, fr, n)) Diagnostics::recordFrame("TX", fr, n);
        }
        s_stat.grants++;
        CdChanger::notePolled();
        return;
    }

    // --- ANYONE? 18 10 01 02 ---
    if (rad == ADDR_BROADCAST && fromMaster && c1 == 0x01 && c2 == 0x02) {
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
        // 0x3B (wewnetrzny CD radia) tez lezy w grupie 0x3X — przydzial jest nasz
        // tylko tuz po naszej odpowiedzi na ANYONE? albo dla naszego adresu.
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

    // --- Rozglaszane informacje radia ---
    if (rad == ADDR_BROADCAST) {
        if (fromMaster && c1 == 0x08 && c2 == 0x00) {
            CdChanger::stopSeekScan();   // puszczenie klawisza FF/REW
        } else if (c1 == 0x87 && (tad == ADDR_MASTER || tad == 0x11)) {
            // Bit 0 = wyjscie audio zmieniarki wlaczone. Prawdziwa zmieniarka po
            // 87 6A / 87 2A przechodzi w status 0x80 (sniff 165726, 16:58:58).
            if ((c2 & 0x01) == 0) {
                s_selected = false;
                stopByRadio("87 audio off");
            }
        } else if (fromMaster && c1 == 0xF0) {
            // Zmiana zrodla: CMD2 = nowe zrodlo (18 10 F0 31 ... = my, F0 51 = tuner).
            if (c2 == s_myAddr) {
                s_selected = true;
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
}

void loadNextSlot(unsigned long now)
{
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
        s_slotKind = SlotKind::Queued;
        s_slotCarriesScreen = (item.len == 16 && item.bytes[2] == 0xC0);
        if (s_slotCarriesScreen) s_slotKey = keyFromC0(item.bytes);
        return;
    }
    if (needDisplayFrame(now)) {
        loadDisplaySlot(now);
    } else if (s_slotKind != SlotKind::None) {
        UnilinkBus::clearGrant();
        s_slotKind = SlotKind::None;
    }
}

bool displaySlotStale()
{
    const DisplayKey k = keyNow();
    if (k.disc != s_slotKey.disc || k.track != s_slotKey.track || k.status != s_slotKey.status) return true;
    if (s_slotKey.seconds == 0xFF && k.seconds != 0xFF) return true;
    if (s_slotKey.seconds == 0xFF) return false;
    return k.minutes != s_slotKey.minutes || k.seconds != s_slotKey.seconds;
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
            settleTextFlag();
        }
        s_slotKind = SlotKind::None;
    }
}

void maybeBreak(unsigned long now)
{
    if (s_breakAwaitingPoll) {
        if (now - s_breakDoneMs < BREAK_POLL_WAIT_MS) return;
        // Impuls poszedl, a master nie odpowiedzial 01 15.
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
            s_slotKind = SlotKind::None;
        }
        s_breakAwaitingPoll = false;
        return;
    }
    // Po zgloszeniu (82 xx) grant przychodzi ~10-30 ms pozniej — slotu wtedy
    // nie ruszamy.
    const bool claimInFlight = (s_slotKind != SlotKind::None) &&
                               UnilinkBus::claimsSent() != s_slotClaimBase &&
                               (now - s_lastClaimMs) < CLAIM_GRANT_WAIT_MS;
    if (!claimInFlight) {
        if (s_slotKind == SlotKind::None ||
            (s_slotKind == SlotKind::Display && !s_txQueue.isEmpty())) {
            // Czas ma pierwszenstwo przed CD-TEXT: inaczej --:-- zostaje na
            // czas bloku nazw, a radio nigdy nie dostaje 0x90.
            const bool timeDue = needDisplayFrame(now) &&
                                 CdChanger::mechState() == MechState::Playing &&
                                 s_txQueue.countPriority(Tx::PRIO_STATUS) == 0;
            if (timeDue && !s_txQueue.isEmpty()) loadDisplaySlot(now);
            else loadNextSlot(now);
        } else if (s_slotKind == SlotKind::Display) {
            if (CdChanger::mechState() == MechState::Init) {
                UnilinkBus::clearGrant();
                s_slotKind = SlotKind::None;
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
            s_busOffSinceMs = 0;
            s_busOnSinceMs  = now;
            s_busOffStopped = false;
            s_lastFrameMs   = now;
            s_lastOwnPingMs = now;
            CdChanger::wake();
            Serial.println("=== BUS_ON = 1 ===");
        } else {
            s_busOffSinceMs = now;
            UnilinkBus::abortTx();
            UnilinkBus::clearGrant();
            s_slotKind = SlotKind::None;
            s_breakAwaitingPoll = false;
            Serial.println("=== BUS_ON = 0 (sesja zachowana) ===");
        }
    }
    // Radio wylaczone na dluzej niz faza BUS_ON=0 w jego discovery (~220 ms):
    // zatrzymujemy muzyke, ale adres zostaje — po wlaczeniu radio czesto pinguje
    // nas pod tym samym adresem.
    if (!s_busActive && !s_busOffStopped && s_busOffSinceMs != 0 &&
        now - s_busOffSinceMs >= BUS_OFF_AUDIO_STOP_MS) {
        s_busOffStopped = true;
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
    if (!s_busActive) return;
    if (!s_silentReported && s_lastFrameMs != 0 && now - s_lastFrameMs > RADIO_SILENT_MS) {
        s_silentReported = true;
        Serial.println(">> Radio milczy (BUS_ON=1, brak ramek)");
        if (millis() > CRASHLOG_GRACE_MS) Serial.captureCrashSnapshot("RADIO SILENT (BUS_ON=1)");
    }
    // Radio aktywnie odpytuje inne urzadzenia, a nas od dawna nie pinguje: zapomnialo
    // o nas bez resetu magistrali. Wracamy do stanu "nieprzydzielona" (z nasluchem
    // pod starym adresem), zeby dolaczyc ramka 10 18 04 00.
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
        CdChanger::serviceAutoAdvance();
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
