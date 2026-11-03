// =============================================================================
// CdChanger.cpp — model wirtualnej zmieniarki CD + koordynacja z audio + NVS
// =============================================================================
#include "CdChanger.h"
#include "Config.h"
#include "AudioPlayer.h"
#include "Diagnostics.h"
#include "UnilinkProtocol.h"
#include <Preferences.h>

namespace CdChanger {

// --- STAN ODTWARZANIA ---
static uint8_t playSeconds = 0;
static uint8_t playMinutes = 0;
static uint8_t currentDisk  = 1;
static uint8_t currentTrack = 1;
// Znacznik millis() odpowiadajacy pozycji 00:00 biezacego utworu. Czas
// odtwarzania liczymy jako (now - playBaseMs)/1000 — monotonicznie i ODPORNIE
// na jitter petli (nie gubi ani nie podwaja sekund, gdy iteracja sie spozni).
static unsigned long playBaseMs = 0;

// --- MASZYNA STANOW ---
static MechState     cdState      = MechState::Init;
static unsigned long seekStartTime = 0;
static unsigned long initWaitTime  = 0;   // 0 = jeszcze nie bylo pierwszego PINGa
static unsigned long audioSilenceSinceMs = 0;

// Ile razy radio odpytalo nas o status/ekran, odkad weszlismy w biezacy stan.
static uint16_t pollsInState = 0;

static inline void enterState(MechState s) {
    cdState = s;
    pollsInState = 0;
}

// --- FLAGA AKTUALIZACJI WYSWIETLACZA ---
static bool needDisplayUpdate = false;

// --- TRYBY ODTWARZANIA (Repeat / Shuffle / Intro) — Wymaganie 8 ---
static PlayModes playModesState = { RepeatMode::Off, false, false };

// --- PRZEWIJANIE — CIAGLE SKANOWANIE CUE/REVIEW (FF/REW) ---
static int           seekScanDir   = 0;
static unsigned long seekScanStart = 0;
static uint32_t      seekAnchorSec = 0;  // pozycja w chwili nacisniecia klawisza
static unsigned long lastAudioSeek = 0;  // ostatni setTimeOffset podczas skanu

// --- PAMIEC NIEULOTNA (NVS) ---
static Preferences prefs;
static uint8_t  lastSavedDisk   = 0;
static uint8_t  lastSavedTrack  = 0;
static uint32_t lastSavedSec    = 0;
// Pozycja, od ktorej wznowimy odtwarzanie (po starcie albo po zatrzymaniu przez radio).
static volatile uint32_t resumeSecOnBoot = 0;
static volatile bool persistPending = false;
static bool alreadyAsleep  = false;
static unsigned long lastPeriodicPersist = 0;
constexpr unsigned long PERIODIC_PERSIST_MS = 30000;

// ============================================================
// NVS
// ============================================================
static void doPersist(uint32_t sec = 0) {
    persistPending = false;
    if (currentDisk == lastSavedDisk && currentTrack == lastSavedTrack && sec == lastSavedSec) return;
    Diagnostics::recordNote("FLASH");
    prefs.putUChar("disk", currentDisk);
    prefs.putUChar("track", currentTrack);
    prefs.putUInt("sec", sec);
    Diagnostics::recordNote("FLEND");
    lastSavedDisk  = currentDisk;
    lastSavedTrack = currentTrack;
    lastSavedSec   = sec;
    Serial.printf("[NVS] Zapamietano: CD%d TR%d, sekunda=%lu\n",
                  currentDisk, currentTrack, (unsigned long)sec);
}

static void loadLast() {
    uint8_t d = prefs.getUChar("disk", 1);
    uint8_t t = prefs.getUChar("track", 1);
    uint32_t s = prefs.getUInt("sec", 0);
    if (d < 1 || d > MAX_DISC) d = 1;
    if (t < 1) t = 1;
    currentDisk  = d;
    currentTrack = t;
    resumeSecOnBoot = s;
    lastSavedDisk  = d;
    lastSavedTrack = t;
    lastSavedSec   = s;
    Serial.printf("[NVS] Wczytano ostatni utwor: CD%d TR%d, sekunda=%lu\n",
                  currentDisk, currentTrack, (unsigned long)resumeSecOnBoot);
}

// ============================================================
// Wejscie w faze ladowania/szukania + start odtwarzania pliku
// ============================================================
static bool loadDiscChanged = false;

static void enterSeek(bool discChanged = false) {
    alreadyAsleep = false;   // ponowne zatrzymanie przez radio musi znow zadzialac
    loadDiscChanged = discChanged;
    enterState(discChanged ? MechState::LoadingTrack : MechState::ChangedCd);
    seekStartTime = millis();
    audioSilenceSinceMs = 0;

    // Jezeli wybrana plyta jest pusta, a indeks USB jest juz gotowy, przechodzimy
    // na pierwsza niepusta plyte.
    if (audioIndexReady() && audioGetTrackCount(currentDisk) == 0 && !Diagnostics::hasError()) {
        uint8_t nextDisc = audioFindNextNonEmptyDisc(currentDisk);
        if (nextDisc != 0) {
            currentDisk  = nextDisc;
            currentTrack = 1;
            resumeSecOnBoot = 0;
        }
    }
    if (audioIndexReady() && audioGetTrackCount(currentDisk) > 0 &&
        currentTrack > audioGetTrackCount(currentDisk)) {
        currentTrack = 1;
        resumeSecOnBoot = 0;
    }

    uint32_t startSec = resumeSecOnBoot;

    playSeconds = (uint8_t)(startSec % 60);
    playMinutes = (uint8_t)((startSec / 60) % 100);
    playBaseMs  = millis() - (unsigned long)startSec * 1000UL;
    needDisplayUpdate = true;

    persistPending = true;  // zapamietamy wybor, gdy magistrala bedzie bezczynna

    if (audioPlayTrack(currentDisk, currentTrack, startSec)) {
        resumeSecOnBoot = 0; // zuzyj dopiero gdy nosnik faktycznie przyjal zlecenie
    } else {
        Serial.printf("[Audio] Nosnik jeszcze niegotowy lub brak pliku CD%02d/%02d — czekam na USB...\n",
                      currentDisk, currentTrack);
    }
}

// ============================================================
// API
// ============================================================
void begin() {
    prefs.begin(PREFS_NAMESPACE, false);
    loadLast();
}

static void enterPlaying(unsigned long now) {
    enterState(MechState::Playing);
    audioSilenceSinceMs = 0;
    if (Diagnostics::hasError()) {
        playMinutes = 99;
        playSeconds = 1;
        playBaseMs  = now;
    } else {
        uint32_t plannedSec = (uint32_t)playMinutes * 60u + (uint32_t)playSeconds;
        uint32_t decoderSec = audioIsPlaying() ? audioGetCurrentTimeSec() : 0;
        // Jezeli wznawiamy od zapamietanej pozycji (plannedSec > 0), a dekoder
        // jeszcze wykonuje asynchroniczny skok setTimeOffset (decoderSec == 0),
        // zachowujemy plannedSec zamiast zerowac licznik do 00:00!
        uint32_t t = (decoderSec > 0) ? decoderSec : plannedSec;
        playMinutes = (uint8_t)((t / 60) % 100);
        playSeconds = (uint8_t)(t % 60);
        playBaseMs  = now - (unsigned long)t * 1000UL;
    }
    needDisplayUpdate = true;
}

void update(unsigned long now, bool radioEngaged, bool selected) {
    // Jeśli wystąpił błąd (np. brak pendrive'a) i radio ma nas jako zrodlo,
    // wchodzimy w tryb Playing z licznikiem 99:01..99:10, aby nadawać CD-TEXT błędu
    // dokładnie tak jak zwykłą piosenkę.
    if (radioEngaged && selected && Diagnostics::hasError() &&
        millis() >= USB_ERROR_GRACE_MS) {
        if (cdState == MechState::Init || cdState == MechState::Idle) {
            enterPlaying(now);
            Serial.println(">>> CdChanger: wejscie w Playing (Error Mode 99:01..99:10)");
        }
    } else {
        // C0 -> 80 (po INIT_DURATION_MS od pierwszego PINGa, gdy sesja aktywna).
        // Jezeli radio wybralo nas jako zrodlo (selected == true), automatycznie
        // ruszamy z odtwarzaniem (nawet jesli radio pominelo komzy 0x20 PLAY po
        // wznowieniu adresu z NVS lub wyslalo tylko 87 29 Audio ON)!
        if (cdState == MechState::Init && radioEngaged && initWaitTime != 0 &&
            (now - initWaitTime > INIT_DURATION_MS)) {
            if (selected && !alreadyAsleep && audioIndexReady()) {
                Serial.println(">>> Init zakonczone (selected=true) -> auto-start odtwarzania!");
                enterSeek(false);
            } else {
                enterState(MechState::Idle);
                needDisplayUpdate = true;
                Serial.println(">>> C0 -> 80 (Idle)");
            }
        } else if (cdState == MechState::Idle && radioEngaged && selected &&
                   !alreadyAsleep && audioIndexReady()) {
            Serial.println(">>> Idle + selected=true + audioIndexReady -> auto-start odtwarzania!");
            enterSeek(false);
        }
    }

    // 0x40 -> [0x20] -> 0x00 (LoadingTrack -> [ChangedCd] -> Playing).
    if (cdState == MechState::LoadingTrack &&
        ((now - seekStartTime > LOAD_DURATION_MS && pollsInState > 0) ||
         (now - seekStartTime > 500))) {
        if (loadDiscChanged) {
            enterState(MechState::ChangedCd);
            seekStartTime = millis();
            needDisplayUpdate = true;
            Serial.println(">>> 40 -> 20 (ChangedCd)");
        } else {
            enterPlaying(now);
            Serial.println(">>> 40 -> 00 (Playing!)");
        }
    } else if (cdState == MechState::ChangedCd &&
               ((now - seekStartTime > SEEK_DURATION_MS && pollsInState > 0) ||
                (now - seekStartTime > 500))) {
        enterPlaying(now);
        Serial.println(">>> 20 -> 00 (Playing!)");
    }

    // 0x21 -> 0x00 (Seeking -> Playing) — po zatrzymaniu przewijania
    if (cdState == MechState::Seeking && seekScanDir == 0) {
        enterState(MechState::Playing);
        needDisplayUpdate = true;
        Serial.printf(">>> 21 -> 00 (Seeking -> Playing!) pozycja %02d:%02d\n",
                      playMinutes, playSeconds);
    }

    if (cdState == MechState::Playing) {
        if (Diagnostics::hasError()) {
            uint32_t elapsed = (now - playBaseMs) / 1000;
            uint8_t newMin = 99;
            uint8_t newSec = 1 + (uint8_t)(elapsed % 10);  // 1..10 (01..10)
            if (newSec != playSeconds || newMin != playMinutes) {
                playSeconds = newSec;
                playMinutes = newMin;
                needDisplayUpdate = true;
            }
        } else {
            // Samonaprawa: jezeli jestesmy w stanie Playing, indeks USB jest gotowy,
            // a dekoder audio nie gra (np. komenda PLAY przyszla zanim pendrive dokonczyl
            // montowanie), ponawiamy uruchomienie utworu od biezacej sekundy!
            if (radioEngaged && selected && audioIndexReady() &&
                !audioIsPlaying() && !audioIsBusyChangingTrack()) {
                if (audioSilenceSinceMs == 0) {
                    audioSilenceSinceMs = now ? now : 1;
                } else if (now - audioSilenceSinceMs >= 600) {
                    audioSilenceSinceMs = now;
                    uint32_t sec = resumeSecOnBoot ? (uint32_t)resumeSecOnBoot
                                                   : ((uint32_t)playMinutes * 60u + (uint32_t)playSeconds);
                    Serial.printf(">>> [Auto-Resume] Stan Playing bez audio — uruchamiam CD%d TR%d (%lus)\n",
                                  currentDisk, currentTrack, (unsigned long)sec);
                    if (audioPlayTrack(currentDisk, currentTrack, sec)) {
                        resumeSecOnBoot = 0;
                        playBaseMs = now - sec * 1000UL;
                    }
                }
            } else {
                audioSilenceSinceMs = 0;
            }

            uint32_t elapsed = (now - playBaseMs) / 1000;
            uint8_t newMin = (uint8_t)((elapsed / 60) % 100);
            uint8_t newSec = (uint8_t)(elapsed % 60);
            if (newSec != playSeconds || newMin != playMinutes) {
                playSeconds = newSec;
                playMinutes = newMin;
                needDisplayUpdate = true;  // co sekunde — nowy czas do nadania
            }
        }
    }
}

void serviceAutoAdvance() {
    if (!audioSongFinished()) return;

    uint8_t maxTr = audioGetTrackCount(currentDisk);
    if (maxTr == 0) maxTr = MAX_TRACK_PER_DISC;

    // --- ZASADA REPEAT (Wymaganie 8.5) ---
    uint8_t prevDisk  = currentDisk;
    uint8_t prevTrack = currentTrack;

    NextTrackResult next = modelNextTrack(playModesState, currentDisk, currentTrack,
                                          MAX_DISC, maxTr);
    bool moved = (next.nextDisc != prevDisk) || (next.nextTrack != prevTrack);
    currentDisk  = next.nextDisc;
    currentTrack = next.nextTrack;

    bool stop = (playModesState.repeat == RepeatMode::Off) && !moved;
    if (stop) {
        enterState(MechState::Idle);      // koniec listy — zatrzymanie mechanizmu
        needDisplayUpdate = true;
        Serial.println(">> AUTO-NEXT: koniec (Repeat Off, ostatni utwor) — STOP");
    } else {
        enterSeek(/*discChanged=*/currentDisk != prevDisk);
        Serial.printf(">> AUTO-NEXT: CD%d TR%d (repeat=%d, moved=%d)\n",
                      currentDisk, currentTrack, (int)playModesState.repeat, moved ? 1 : 0);
    }
}

void serviceMediaMount() {
    static bool wasMounted = false;
    bool isMounted = audioGetTrackCount(currentDisk) > 0 ||
                     audioFindNextNonEmptyDisc(0) > 0;

    if (isMounted && !wasMounted) {
        wasMounted = true;
        if (audioGetTrackCount(currentDisk) > 0) {
            uint8_t maxTr = audioGetTrackCount(currentDisk);
            if (currentTrack > maxTr) {
                currentTrack = 1;
                resumeSecOnBoot = 0;
            }
            Serial.printf(">>> Wykryto USB. Wznawiam zapamietana plyte: CD%d TR%d\n",
                          currentDisk, currentTrack);
        } else {
            uint8_t firstDisc = audioFindNextNonEmptyDisc(0);
            if (firstDisc != 0) {
                currentDisk = firstDisc;
                currentTrack = 1;
                resumeSecOnBoot = 0;
                Serial.printf(">>> Wykryto USB. Ustawiono pierwsza niepusta plyte: CD%d\n",
                              currentDisk);
            }
        }

        // --- AUTOMATYCZNY POWRÓT DO GRY PO HOT-PLUG / STARCIE USB ---
        if (!alreadyAsleep &&
            (cdState == MechState::Playing || cdState == MechState::LoadingTrack ||
             cdState == MechState::ChangedCd ||
             (UnilinkProtocol::isAllocated() && UnilinkProtocol::isSelected()))) {
            if (!audioIsPlaying() && !audioIsBusyChangingTrack()) {
                Serial.println(">>> USB gotowe: Wznawiam odtwarzanie (enterSeek)!");
                enterSeek(/*discChanged=*/false);
            }
        }
    } else if (!isMounted && wasMounted) {
        wasMounted = false;
    }
}

void handlePlayCommand() {
    alreadyAsleep = false;
    if (audioIsPlaying() || audioIsBusyChangingTrack()) {
        if (cdState != MechState::Playing && cdState != MechState::LoadingTrack &&
            cdState != MechState::ChangedCd) {
            uint32_t t = audioIsPlaying() ? audioGetCurrentTimeSec()
                                          : ((uint32_t)playMinutes * 60u + (uint32_t)playSeconds);
            playMinutes = (uint8_t)((t / 60) % 100);
            playSeconds = (uint8_t)(t % 60);
            playBaseMs = millis() - (unsigned long)t * 1000UL;
            enterState(MechState::Playing);
            needDisplayUpdate = true;
            Serial.printf(">> PLAY (wznowienie bez restartu: %02d:%02d)\n",
                          playMinutes, playSeconds);
        } else {
            Serial.println(">> PLAY (juz gram — ignoruje, nie restartuje utworu)");
        }
    } else {
        Serial.println(">> PLAY command received!");
        enterSeek(/*discChanged=*/true);
    }
}

void nextTrack() {
    stopSeekScan();
    uint8_t maxTr = audioGetTrackCount(currentDisk);
    if (maxTr == 0) maxTr = MAX_TRACK_PER_DISC;
    if (currentTrack < maxTr) {
        currentTrack++;
        resumeSecOnBoot = 0;
        enterSeek(false);
        Serial.printf(">> NEXT TRACK: CD%d TR%d (max=%d)\n", currentDisk, currentTrack, maxTr);
    } else {
        // Koniec plyty -> przejscie na nastepne niepuste CD (lub zawiniecie do CD01)
        uint8_t prevDisc = currentDisk;
        uint8_t nextNonEmpty = audioFindNextNonEmptyDisc(currentDisk);
        if (nextNonEmpty != 0) {
            currentDisk = nextNonEmpty;
        } else {
            currentDisk++;
            if (currentDisk > MAX_DISC) currentDisk = 1;
        }
        currentTrack = 1;
        resumeSecOnBoot = 0;
        const bool discChanged = (currentDisk != prevDisc);
        enterSeek(discChanged);
        Serial.printf(">> NEXT TRACK (koniec CD%d -> CD%d TR%d)\n",
                      prevDisc, currentDisk, currentTrack);
    }
}

void prevTrack() {
    stopSeekScan();
    uint32_t currentSec = (uint32_t)playMinutes * 60 + playSeconds;
    resumeSecOnBoot = 0;

    if (currentSec > 2) {
        enterSeek(false);
        Serial.printf(">> PREV TRACK (Restart utworu): CD%d TR%d\n", currentDisk, currentTrack);
    } else if (currentTrack > 1) {
        currentTrack--;
        enterSeek(false);
        Serial.printf(">> PREV TRACK: CD%d TR%d\n", currentDisk, currentTrack);
    } else {
        // Poczatek plyty (TR01, czas <= 2s) -> przejscie na poprzednie niepuste CD na ostatni utwor
        uint8_t prevDisc = currentDisk;
        uint8_t prevNonEmpty = audioFindPrevNonEmptyDisc(currentDisk);
        if (prevNonEmpty != 0) {
            currentDisk = prevNonEmpty;
        } else if (currentDisk <= 1) {
            currentDisk = MAX_DISC;
        } else {
            currentDisk--;
        }

        uint8_t lastTr = audioGetTrackCount(currentDisk);
        if (lastTr == 0) lastTr = 1;
        currentTrack = lastTr;
        resumeSecOnBoot = 0;
        const bool discChanged = (currentDisk != prevDisc);
        enterSeek(discChanged);
        Serial.printf(">> PREV TRACK (poczatek CD%d -> CD%d TR%d [ostatni])\n",
                      prevDisc, currentDisk, currentTrack);
    }
}

void nextDisc() {
    stopSeekScan();
    uint8_t nextNonEmpty = audioFindNextNonEmptyDisc(currentDisk);
    if (nextNonEmpty != 0) {
        currentDisk = nextNonEmpty;
    } else {
        currentDisk++;
        if (currentDisk > MAX_DISC) currentDisk = 1;
    }
    currentTrack = 1;
    resumeSecOnBoot = 0;
    enterSeek(/*discChanged=*/true);
    Serial.printf(">> NEXT DISC: CD%d\n", currentDisk);
}

void prevDisc() {
    stopSeekScan();
    uint8_t prevNonEmpty = audioFindPrevNonEmptyDisc(currentDisk);
    if (prevNonEmpty != 0) {
        currentDisk = prevNonEmpty;
    } else if (currentDisk <= 1) {
        currentDisk = MAX_DISC;
    } else {
        currentDisk--;
    }
    currentTrack = 1;
    resumeSecOnBoot = 0;
    enterSeek(/*discChanged=*/true);
    Serial.printf(">> PREV DISC: CD%d\n", currentDisk);
}

static void setPlayPosition(uint32_t sec) {
    uint8_t m = (uint8_t)((sec / 60) % 100);
    uint8_t s = (uint8_t)(sec % 60);
    playBaseMs = millis() - (unsigned long)sec * 1000UL;
    if (m != playMinutes || s != playSeconds) {
        playMinutes = m;
        playSeconds = s;
        needDisplayUpdate = true;
    }
}

// Ile sekund materialu przeskanowano po `elapsedMs` trzymania klawisza FF/REW.
// Startujemy od 1 s od razu w chwili nacisniecia klawisza (tak jak oryginalna
// zmieniarka Sony, zeby nawet krotkie przytrzymanie od razu przesuwalo czas),
// a dalej trzystopniowe przyspieszenie.
static uint32_t scanDistanceSec(unsigned long elapsedMs) {
    unsigned long t = elapsedMs;
    uint32_t dist = 1;

    unsigned long seg = (t < SCAN_PHASE1_MS) ? t : SCAN_PHASE1_MS;
    dist += (uint32_t)(seg * SCAN_RATE1 / 1000);
    t -= seg;

    if (t > 0) {
        seg = (t < SCAN_PHASE2_MS) ? t : SCAN_PHASE2_MS;
        dist += (uint32_t)(seg * SCAN_RATE2 / 1000);
        t -= seg;
    }
    if (t > 0) {
        dist += (uint32_t)(t * SCAN_RATE3 / 1000);
    }
    return dist;
}

static void syncAudioToDisplayClock() {
    uint32_t target = (uint32_t)playMinutes * 60u + (uint32_t)playSeconds;
    audioSeekToSec(target);
    lastAudioSeek = millis();
}

static void endSeekScan() {
    seekScanDir = 0;
    needDisplayUpdate = true;
    syncAudioToDisplayClock();
    audioSetInfoSquelch(false);
    audioSetSeekMute(false);
}

void seek(int deltaSec) {
    if (cdState != MechState::Playing && cdState != MechState::Seeking) return;
    const int dir = (deltaSec >= 0) ? +1 : -1;
    const unsigned long now = millis();

    if (seekScanDir == dir) return;

    seekAnchorSec = (uint32_t)playMinutes * 60 + playSeconds;
    seekScanDir   = dir;
    seekScanStart = now;
    lastAudioSeek = now;

    if (cdState != MechState::Seeking) {
        enterState(MechState::Seeking);
        audioSetInfoSquelch(true);
        audioSetSeekMute(true);
    }

    // Natychmiast przesun licznik o pierwsza sekunde, aby ekran zareagowal bez zwloki
    long target = (long)seekAnchorSec + (long)dir;
    if (target < 0) target = 0;
    const uint32_t dur = audioGetDurationSec();
    if (dur > 1 && target > (long)(dur - 1)) target = (long)(dur - 1);
    setPlayPosition((uint32_t)target);

    needDisplayUpdate = true;
    Serial.printf(">> SCAN %s start: CD%d TR%d %02d:%02d\n",
                  dir > 0 ? "FF" : "REW", currentDisk, currentTrack,
                  playMinutes, playSeconds);
}

void serviceSeekRepeat(unsigned long now) {
    if (seekScanDir == 0) return;

    if (cdState != MechState::Playing && cdState != MechState::Seeking) {
        endSeekScan();
        return;
    }

    if (now - seekScanStart > SEEK_SCAN_MAX_MS) {
        Serial.printf(">> SCAN auto-stop (limit %lus): CD%d TR%d %02d:%02d\n",
                      SEEK_SCAN_MAX_MS / 1000, currentDisk, currentTrack,
                      playMinutes, playSeconds);
        endSeekScan();
        return;
    }

    long target = (long)seekAnchorSec +
                  (long)seekScanDir * (long)scanDistanceSec(now - seekScanStart);
    const uint32_t dur = audioGetDurationSec();

    if (target < 0) {
        target = 0;
    } else if (dur > 2 && target >= (long)(dur - 1)) {
        // Prawdziwa zmieniarka po przewinieciu do konca utworu przechodzi na nastepny utwor
        uint8_t maxTr = audioGetTrackCount(currentDisk);
        if (maxTr == 0) maxTr = MAX_TRACK_PER_DISC;
        if (currentTrack < maxTr) {
            endSeekScan();
            currentTrack++;
            resumeSecOnBoot = 0;
            enterSeek(false);
            Serial.printf(">> SCAN FF koniec utworu -> NEXT TRACK CD%d TR%d\n", currentDisk, currentTrack);
            return;
        }
        target = (long)(dur - 1);
    }
    setPlayPosition((uint32_t)target);

    // Audio pozostaje wyciszone podczas trzymania klawisza (brak szarpania i trzaskow).
    // Pojedynczy, czysty skok nastapi w endSeekScan() po puszczeniu klawisza.
}

void stopSeekScan() {
    if (seekScanDir != 0) {
        Serial.printf(">> SCAN stop (0x08 0x00): CD%d TR%d %02d:%02d\n",
                      currentDisk, currentTrack, playMinutes, playSeconds);
        endSeekScan();
    }
}

void selectDiscTrack(uint8_t disc, uint8_t track) {
    stopSeekScan();
    const bool discChanged = (disc != currentDisk);
    currentDisk  = disc;
    currentTrack = track;
    resumeSecOnBoot = 0;
    enterSeek(discChanged);
    needDisplayUpdate = true;
}

void resetToInit() {
    enterState(MechState::Init);
    initWaitTime = 0;
}

void resetToAllocated() {
    // Czas dekodera dla VBR jest tylko szacunkiem — przestawienie na niego zegara,
    // ktory juz biegnie, daje na ekranie skok o kilka sekund.
    if (cdState == MechState::Playing && audioIsPlaying()) {
        needDisplayUpdate = true;
        Serial.printf(">> resetToAllocated: gram dalej %02d:%02d\n", playMinutes, playSeconds);
        return;
    }
    if (audioIsPlaying() || audioIsBusyChangingTrack()) {
        uint32_t t = audioIsPlaying() ? audioGetCurrentTimeSec()
                                      : ((uint32_t)playMinutes * 60u + (uint32_t)playSeconds);
        playMinutes = (uint8_t)((t / 60) % 100);
        playSeconds = (uint8_t)(t % 60);
        playBaseMs = millis() - (unsigned long)t * 1000UL;
        if (cdState != MechState::Playing) {
            enterState(MechState::Playing);
        }
        needDisplayUpdate = true;
        Serial.printf(">> resetToAllocated: audio gra, zachowuje Playing %02d:%02d\n",
                      playMinutes, playSeconds);
    } else {
        resetToInit();
    }
}

void toggleRepeat() {
    switch (playModesState.repeat) {
        case RepeatMode::Off: playModesState.repeat = RepeatMode::One; break;
        case RepeatMode::One: playModesState.repeat = RepeatMode::All; break;
        case RepeatMode::All: playModesState.repeat = RepeatMode::Off; break;
    }
    needDisplayUpdate = true;
    enqueueModeIcons();
    Serial.printf(">> TOGGLE REPEAT -> %d\n", (int)playModesState.repeat);
}

void toggleShuffle() {
    playModesState.shuffle = !playModesState.shuffle;
    needDisplayUpdate = true;
    enqueueModeIcons();
    Serial.printf(">> TOGGLE SHUFFLE -> %d\n", playModesState.shuffle ? 1 : 0);
}

void toggleIntro() {
    playModesState.intro = !playModesState.intro;
    needDisplayUpdate = true;
    enqueueModeIcons();
    Serial.printf(">> TOGGLE INTRO -> %d\n", playModesState.intro ? 1 : 0);
}

void noteFirstPing() {
    if (initWaitTime == 0) {
        initWaitTime = millis();
    }
}

void notePolled() {
    if (pollsInState < 0xFFFF) pollsInState++;
}

void sleep() {
    if (alreadyAsleep) return;
    alreadyAsleep = true;

    seekScanDir = 0;
    audioSetInfoSquelch(false);
    if (audioIsPlaying()) {
        resumeSecOnBoot = audioGetCurrentTimeSec();
    } else if (cdState == MechState::Playing) {
        resumeSecOnBoot = (uint32_t)playMinutes * 60u + (uint32_t)playSeconds;
    }
    persistPending = true;
    audioStop();
    enterState(MechState::Init);
    initWaitTime = 0;
    needDisplayUpdate = false;
}

static uint32_t positionToPersist() {
    return audioIsPlaying() ? audioGetCurrentTimeSec() : resumeSecOnBoot;
}

void servicePersist(bool flashWriteAllowed) {
    if (!flashWriteAllowed) return;
    const unsigned long now = millis();
    if (persistPending) {
        persistPending = false;
        doPersist(positionToPersist());
        lastPeriodicPersist = now;
        return;
    }
    if (cdState == MechState::Playing && audioIsPlaying() &&
        now - lastPeriodicPersist > PERIODIC_PERSIST_MS) {
        lastPeriodicPersist = now;
        doPersist(audioGetCurrentTimeSec());
    }
}

void persistNow() {
    persistPending = false;
    doPersist(positionToPersist());
}

void wake() {
    alreadyAsleep = false;
    audioSetVolume(AUDIO_VOLUME);
}

void enqueueModeIcons() {
    UnilinkProtocol::enqueueModeIconsHelper((uint8_t)playModesState.repeat,
                                            playModesState.shuffle,
                                            playModesState.intro);
}

MechState mechState() { return cdState; }
bool isSeeking() { return cdState == MechState::Seeking; }

uint8_t statusByte() { return UnilinkFrame::statusByte(cdState); }

uint8_t disk()     { return currentDisk; }
uint8_t track()    { return currentTrack; }
uint8_t minutes()  { return playMinutes; }
uint8_t seconds()  { return playSeconds; }

void setDisk(uint8_t disc) {
    if (disc >= 1 && disc <= MAX_DISC) {
        currentDisk = disc;
    }
}
void setTrack(uint8_t track) {
    if (track >= 1) {
        currentTrack = track;
    }
}

PlayModes  playModes()  { return playModesState; }
RepeatMode repeatMode() { return playModesState.repeat; }
bool       shuffle()    { return playModesState.shuffle; }
bool       intro()      { return playModesState.intro; }

bool isDisplayDirty()  { return needDisplayUpdate; }
void clearDisplayDirty() { needDisplayUpdate = false; }

NextTrackResult modelNextTrack(PlayModes modes, uint8_t disc, uint8_t track,
                               uint8_t maxDisc, uint8_t maxTrack) {
    NextTrackResult result = {disc, track};
    
    switch (modes.repeat) {
        case RepeatMode::One:
            break;
            
        case RepeatMode::All:
            if (track < maxTrack) {
                result.nextTrack = track + 1;
            } else {
                result.nextTrack = 1;
                uint8_t nextNonEmpty = audioFindNextNonEmptyDisc(disc);
                if (nextNonEmpty != 0) {
                    result.nextDisc = nextNonEmpty;
                } else {
                    result.nextDisc = (disc >= maxDisc) ? 1 : disc + 1;
                }
            }
            break;
            
        case RepeatMode::Off:
            if (track < maxTrack) {
                result.nextTrack = track + 1;
            } else {
                uint8_t nextNonEmpty = audioFindNextNonEmptyDisc(disc);
                if (nextNonEmpty != 0) {
                    result.nextDisc = nextNonEmpty;
                    result.nextTrack = 1;
                }
            }
            break;
    }
    
    return result;
}

} // namespace CdChanger
