// =============================================================================
// CdChanger.cpp — model wirtualnej zmieniarki CD + koordynacja z audio + NVS
// =============================================================================
#include "CdChanger.h"
#include "Config.h"
#include "AudioPlayer.h"
#include <Preferences.h>

namespace CdChanger {

// --- STAN ODTWARZANIA ---
static uint8_t playSeconds = 0;
static uint8_t playMinutes = 0;
static uint8_t currentDisk  = 1;
static uint8_t currentTrack = 1;
static unsigned long lastSecondTick = 0;

// --- MASZYNA STANOW ---
static State         cdState      = STATE_INIT;
static unsigned long seekStartTime = 0;
static unsigned long initWaitTime  = 0;   // 0 = jeszcze nie bylo pierwszego PINGa

// --- FLAGA AKTUALIZACJI WYSWIETLACZA ---
static bool needDisplayUpdate = false;

// --- PAMIEC NIEULOTNA (NVS) ---
// Zapamietuje ostatnio odtwarzany utwor, by po wylaczeniu radia (BUS=0) wznowic
// od tej samej plyty/utworu.
static Preferences prefs;
static uint8_t lastSavedDisk  = 0;
static uint8_t lastSavedTrack = 0;

// ============================================================
// NVS
// ============================================================
static void persist() {
    // Zapis tylko gdy cos sie zmienilo (oszczedzamy zywotnosc NVS).
    if (currentDisk == lastSavedDisk && currentTrack == lastSavedTrack) return;
    prefs.putUChar("disk", currentDisk);
    prefs.putUChar("track", currentTrack);
    lastSavedDisk  = currentDisk;
    lastSavedTrack = currentTrack;
    Serial.printf("[NVS] Zapamietano ostatni utwor: CD%d TR%d\n", currentDisk, currentTrack);
}

static void loadLast() {
    uint8_t d = prefs.getUChar("disk", 1);
    uint8_t t = prefs.getUChar("track", 1);
    if (d < 1 || d > MAX_DISC) d = 1;
    if (t < 1) t = 1;
    currentDisk  = d;
    currentTrack = t;
    lastSavedDisk  = d;
    lastSavedTrack = t;
    Serial.printf("[NVS] Wczytano ostatni utwor: CD%d TR%d\n", currentDisk, currentTrack);
}

// ============================================================
// Wejscie w faze ladowania/szukania + start odtwarzania pliku
// ============================================================
static void enterSeek() {
    cdState = STATE_LOADING;
    seekStartTime = millis();
    playSeconds = 0;
    playMinutes = 0;
    needDisplayUpdate = true;

    persist();  // zapamietaj wybor (przetrwa wylaczenie radia)

    if (!audioPlayTrack(currentDisk, currentTrack)) {
        Serial.printf("[Audio] Brak pliku CD%02d/%02d — szukam nastepnego...\n",
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

void update(unsigned long now, bool radioEngaged) {
    // C0 -> 80 (po INIT_DURATION_MS od pierwszego PINGa, gdy sesja aktywna)
    if (cdState == STATE_INIT && radioEngaged && initWaitTime != 0 &&
        (now - initWaitTime > INIT_DURATION_MS)) {
        cdState = STATE_IDLE;
        Serial.println(">>> C0 -> 80 (Idle)");
    }

    // 0x40 -> 0x20 -> 0x00 (Loading -> Seeking -> Playing)
    if (cdState == STATE_LOADING && (now - seekStartTime > LOAD_DURATION_MS)) {
        cdState = STATE_SEEKING;
        seekStartTime = millis();
        needDisplayUpdate = true;
        Serial.println(">>> 40 -> 20 (Seeking)");
    } else if (cdState == STATE_SEEKING && (now - seekStartTime > SEEK_DURATION_MS)) {
        cdState = STATE_PLAYING;
        needDisplayUpdate = true;
        lastSecondTick = millis();
        playSeconds = 0;
        playMinutes = 0;
        Serial.println(">>> 20 -> 00 (Playing!)");
    }

    // Sekundnik — wlasny, plynny pomiar czasu co rowno 1000ms (niezalezny od
    // czasu dekodera, ktory potrafil przeskakiwac przy napelnianiu bufora).
    if (cdState == STATE_PLAYING && (now - lastSecondTick >= 1000)) {
        lastSecondTick += 1000;  // bez dryftu
        if (now - lastSecondTick > 1000) lastSecondTick = now;  // zabezpieczenie

        playSeconds++;
        if (playSeconds >= 60) {
            playSeconds = 0;
            playMinutes++;
            if (playMinutes >= 100) playMinutes = 0;
        }
        needDisplayUpdate = true;
    }
}

void serviceAutoAdvance() {
    if (!audioSongFinished()) return;

    uint8_t maxTr = audioGetTrackCount(currentDisk);
    if (maxTr == 0) maxTr = MAX_TRACK_PER_DISC;

    currentTrack++;
    if (currentTrack > maxTr) {
        currentTrack = 1;
        uint8_t nextNonEmpty = audioFindNextNonEmptyDisc(currentDisk);
        if (nextNonEmpty != 0) {
            currentDisk = nextNonEmpty;
        } else {
            currentDisk++;
            if (currentDisk > MAX_DISC) currentDisk = 1;
        }
    }
    enterSeek();
    Serial.printf(">> AUTO-NEXT: CD%d TR%d\n", currentDisk, currentTrack);
}

void serviceMediaMount() {
    static bool wasMounted = false;
    bool isMounted = audioGetTrackCount(currentDisk) > 0 ||
                     audioFindNextNonEmptyDisc(0) > 0;

    if (isMounted && !wasMounted) {
        wasMounted = true;
        // Jesli wczytana z NVS plyta ma utwory — zostajemy na niej (wznawiamy).
        // W przeciwnym razie wybieramy pierwsza niepusta plyte.
        if (audioGetTrackCount(currentDisk) > 0) {
            uint8_t maxTr = audioGetTrackCount(currentDisk);
            if (currentTrack > maxTr) currentTrack = 1;
            Serial.printf(">>> Wykryto USB. Wznawiam zapamietana plyte: CD%d TR%d\n",
                          currentDisk, currentTrack);
        } else {
            uint8_t firstDisc = audioFindNextNonEmptyDisc(0);
            if (firstDisc != 0) {
                currentDisk = firstDisc;
                currentTrack = 1;
                Serial.printf(">>> Wykryto USB. Ustawiono pierwsza niepusta plyte: CD%d\n",
                              currentDisk);
            }
        }
    } else if (!isMounted && wasMounted) {
        wasMounted = false;
    }
}

void handlePlayCommand() {
    // Radio potrafi WIELOKROTNIE wysylac PLAY w trakcie grania. Jesli juz gramy,
    // NIE restartujemy utworu od zera (dotyczy tez sytuacji po SYSTEM RESET radia
    // w trakcie grania: audio nie zostalo zatrzymane).
    if (audioIsPlaying()) {
        if (cdState != STATE_PLAYING) {
            uint32_t t = audioGetCurrentTimeSec();
            playMinutes = t / 60;
            playSeconds = t % 60;
            lastSecondTick = millis();
            cdState = STATE_PLAYING;
            needDisplayUpdate = true;
            Serial.printf(">> PLAY (wznowienie bez restartu, np. po resecie radia: %02d:%02d)\n",
                          playMinutes, playSeconds);
        } else {
            Serial.println(">> PLAY (juz gram — ignoruje, nie restartuje utworu)");
        }
    } else {
        Serial.println(">> PLAY command received!");
        enterSeek();
    }
}

void nextTrack() {
    uint8_t maxTr = audioGetTrackCount(currentDisk);
    if (maxTr == 0) maxTr = MAX_TRACK_PER_DISC;  // fallback bez nosnika
    if (currentTrack < maxTr) {
        currentTrack++;
        enterSeek();
        Serial.printf(">> NEXT TRACK: CD%d TR%d (max=%d)\n", currentDisk, currentTrack, maxTr);
    } else {
        Serial.println(">> NEXT TRACK zablokowany: to ostatni utwor na plycie.");
    }
}

void prevTrack() {
    uint32_t currentSec = playSeconds;
    if (audioIsPlaying()) currentSec = audioGetCurrentTimeSec();

    if (currentSec > 2) {
        // Cofnij na poczatek obecnego utworu
        enterSeek();
        Serial.printf(">> PREV TRACK (Restart utworu): CD%d TR%d\n", currentDisk, currentTrack);
    } else if (currentTrack > 1) {
        currentTrack--;
        enterSeek();
        Serial.printf(">> PREV TRACK: CD%d TR%d\n", currentDisk, currentTrack);
    } else {
        // Na 1 utworze, <= 2s -> cofnij na poczatek pierwszego utworu
        enterSeek();
        Serial.println(">> PREV TRACK (Restart 1 utworu, brak poprzedniego)");
    }
}

void nextDisc() {
    uint8_t nextNonEmpty = audioFindNextNonEmptyDisc(currentDisk);
    if (nextNonEmpty != 0) {
        currentDisk = nextNonEmpty;
    } else {
        currentDisk++;
        if (currentDisk > MAX_DISC) currentDisk = 1;
    }
    currentTrack = 1;
    enterSeek();
    Serial.printf(">> NEXT DISC: CD%d\n", currentDisk);
}

void prevDisc() {
    uint8_t prevNonEmpty = audioFindPrevNonEmptyDisc(currentDisk);
    if (prevNonEmpty != 0) {
        currentDisk = prevNonEmpty;
    } else if (currentDisk <= 1) {
        currentDisk = MAX_DISC;
    } else {
        currentDisk--;
    }
    currentTrack = 1;
    enterSeek();
    Serial.printf(">> PREV DISC: CD%d\n", currentDisk);
}

void resetToInit() {
    cdState = STATE_INIT;
    initWaitTime = 0;
}

void noteFirstPing() {
    if (cdState == STATE_INIT && initWaitTime == 0) {
        initWaitTime = millis();
    }
}

void sleep() {
    // Radio uspione/wylaczone — zatrzymaj dzwiek i zapamietaj ostatni utwor.
    persist();
    audioStop();
    cdState = STATE_INIT;
    initWaitTime = 0;
    needDisplayUpdate = false;
}

void wake() {
    // Pobudka — upewnij sie, ze DAC ma docelowa glosnosc.
    audioSetVolume(AUDIO_VOLUME);
}

State   state()    { return cdState; }
uint8_t disk()     { return currentDisk; }
uint8_t track()    { return currentTrack; }
uint8_t minutes()  { return playMinutes; }
uint8_t seconds()  { return playSeconds; }

bool isDisplayDirty()  { return needDisplayUpdate; }
void clearDisplayDirty() { needDisplayUpdate = false; }

} // namespace CdChanger
