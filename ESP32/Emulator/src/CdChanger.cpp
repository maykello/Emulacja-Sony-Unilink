// =============================================================================
// CdChanger.cpp — model wirtualnej zmieniarki CD + koordynacja z audio + NVS
// =============================================================================
#include "CdChanger.h"
#include "Config.h"
#include "AudioPlayer.h"
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

// Ile razy radio odpytalo nas o status/ekran, odkad weszlismy w biezacy stan.
// Prawdziwa zmieniarka jest wolna: kazdy etap (ladowanie, zmiana plyty) trwa u
// niej dosc dlugo, by radio zdazylo go ODCZYTAC i pokazac na wyswietlaczu.
// Nasz mechanizm jest wirtualny i przelaczalby stany w kilkadziesiat ms —
// szybciej, niz radio zdazy zapytac. Radio widzialoby wtedy tylko stan koncowy,
// przez co ekran nie odswiezal numeru plyty ani napisu LOAD. Dlatego kazde
// przejscie wymaga NIE TYLKO uplywu czasu, ale i tego, by radio przynajmniej
// raz zobaczylo biezacy stan.
static uint16_t pollsInState = 0;

static inline void enterState(MechState s) {
    cdState = s;
    pollsInState = 0;
}

// --- FLAGA AKTUALIZACJI WYSWIETLACZA ---
static bool needDisplayUpdate = false;

// --- TRYBY ODTWARZANIA (Repeat / Shuffle / Intro) — Wymaganie 8 ---
// Domyslnie wszystko wylaczone (Off/false/false), tak jak po wlozeniu plyty.
static PlayModes playModesState = { RepeatMode::Off, false, false };

// --- PRZEWIJANIE — CIAGLE SKANOWANIE CUE/REVIEW (FF/REW) ---
// scanDir: 0 = brak skanowania, +1 = do przodu (FF), -1 = do tylu (REW).
// Nacisniecie klawisza (0x24/0x25) startuje skan, puszczenie (broadcast
// `08 00`) go konczy. Pozycje liczymy CIAGLE z czasu trzymania — patrz
// scanDistanceSec — wiec licznik plynie plynnie i przyspiesza, zamiast skakac
// po rownych porcjach.
static int           seekScanDir   = 0;
static unsigned long seekScanStart = 0;
static uint32_t      seekAnchorSec = 0;  // pozycja w chwili nacisniecia klawisza
static unsigned long lastAudioSeek = 0;  // ostatni setTimeOffset podczas skanu

// --- PAMIEC NIEULOTNA (NVS) ---
// Zapamietuje ostatnio odtwarzany utwor, by po wylaczeniu radia (BUS=0) wznowic
// od tej samej plyty/utworu.
static Preferences prefs;
static uint8_t lastSavedDisk  = 0;
static uint8_t lastSavedTrack = 0;
// Zapis NVS (flash) blokuje petle na ~15-40ms, wiec NIE robimy go w hot-path
// (przy zmianie plyty/utworu). Zamiast tego oznaczamy "do zapisania" i flush
// nastepuje dopiero gdy magistrala jest bezczynna (servicePersist) albo przy
// uspieniu (sleep) — wtedy blokada nikomu nie szkodzi.
static bool persistPending = false;

// ============================================================
// NVS
// ============================================================
static void doPersist() {
    persistPending = false;
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
    enterState(MechState::LoadingTrack);
    seekStartTime = millis();
    playSeconds = 0;
    playMinutes = 0;
    playBaseMs = millis();
    needDisplayUpdate = true;

    persistPending = true;  // zapamietamy wybor, gdy magistrala bedzie bezczynna

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
    if (cdState == MechState::Init && radioEngaged && initWaitTime != 0 &&
        (now - initWaitTime > INIT_DURATION_MS)) {
        enterState(MechState::Idle);
        Serial.println(">>> C0 -> 80 (Idle)");
    }

    // 0x40 -> 0x20 -> 0x00 (LoadingTrack -> ChangedCd -> Playing).
    // Warunek `pollsInState` gwarantuje, ze radio zobaczylo stan posredni —
    // patrz komentarz przy deklaracji pollsInState.
    if (cdState == MechState::LoadingTrack &&
        (now - seekStartTime > LOAD_DURATION_MS) && pollsInState > 0) {
        enterState(MechState::ChangedCd);
        seekStartTime = millis();
        needDisplayUpdate = true;
        Serial.println(">>> 40 -> 20 (ChangedCd)");
    } else if (cdState == MechState::ChangedCd &&
               (now - seekStartTime > SEEK_DURATION_MS) && pollsInState > 0) {
        enterState(MechState::Playing);
        needDisplayUpdate = true;
        playBaseMs = now;       // 00:00 biezacego utworu = teraz
        playSeconds = 0;
        playMinutes = 0;
        Serial.println(">>> 20 -> 00 (Playing!)");
    }

    // 0x21 -> 0x00 (Seeking -> Playing) — po zatrzymaniu przewijania
    // Wymaganie 10.2: broadcast 0x08 0x00 (RAD=0x18) zwraca do Playing.
    // Zatrzymanie skanu zatrzaskowego (seekScanDir=0) tez powinno zwrócic do Playing.
    if (cdState == MechState::Seeking && seekScanDir == 0) {
        enterState(MechState::Playing);
        needDisplayUpdate = true;
        // NIE resetujemy tu czasu! playBaseMs/playMinutes/playSeconds zostaly juz
        // ustawione przez doSeekStep na pozycje, do ktorej przewinieto. Wczesniej
        // bylo tu `playBaseMs = now`, co po przewijaniu zerowalo czas na ekranie
        // (np. przewiniete do 01:09, a wyswietlacz pokazywal 00:00 — rozjazd z
        // faktyczna pozycja audio). Zachowujemy przewinieta pozycje.
        Serial.printf(">>> 21 -> 00 (Seeking -> Playing!) pozycja %02d:%02d\n",
                      playMinutes, playSeconds);
    }

    // Czas odtwarzania liczony z bazowego znacznika — monotonicznie i bez
    // dryftu/przeskokow niezaleznie od jitteru petli.
    //
    // [NAPRAWA ZASTYGANIA] Zmiana sekundy USTAWIA needDisplayUpdate. Dzieki
    // temu isDisplayDirty() jest true co sekunde w Playing, co gwarantuje ze
    // wantsBus() zwraca true i emulator ZAWSZE sie zglasza w arbitrazu 01 15.
    // Prawdziwa zmieniarka co sekunde raportuje nowy czas — nasz emulator tez.
    if (cdState == MechState::Playing) {
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

    // Zatrzymujemy odtwarzanie WYLACZNIE w trybie Repeat Off, gdy nie ma kolejnego
    // utworu (ostatni utwor ostatniej plyty). We wszystkich innych przypadkach
    // ponownie startujemy audio:
    //   - Repeat One  -> ten sam utwor (modelNextTrack zwraca te sama pozycje;
    //                    bez enterSeek utwor po prostu by zamilkl — to byl blad),
    //   - Repeat All  -> nastepny / zawiniety (lub ten sam, gdy jedna plyta/utwor),
    //   - Repeat Off  -> nastepny utwor, dopoki jakis jest.
    bool stop = (playModesState.repeat == RepeatMode::Off) && !moved;
    if (stop) {
        enterState(MechState::Idle);      // koniec listy — zatrzymanie mechanizmu
        needDisplayUpdate = true;
        Serial.println(">> AUTO-NEXT: koniec (Repeat Off, ostatni utwor) — STOP");
    } else {
        enterSeek();
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
        if (cdState != MechState::Playing) {
            uint32_t t = audioGetCurrentTimeSec();
            playMinutes = (uint8_t)((t / 60) % 100);
            playSeconds = (uint8_t)(t % 60);
            playBaseMs = millis() - (unsigned long)t * 1000;  // baza = teraz - pozycja
            enterState(MechState::Playing);
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
    // Uzywamy NASZEGO licznika czasu (nie pozycji dekodera, ktora jest
    // niewiarygodna w trakcie zmiany utworu). Dzieki temu zachowanie jest
    // deterministyczne: w srodku utworu track- restartuje biezacy utwor, a
    // ponowne track- (gdy czas znow ~0) cofa do poprzedniego.
    uint32_t currentSec = (uint32_t)playMinutes * 60 + playSeconds;

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

// Ustaw pozycje odtwarzania (sekundy od poczatku utworu) na naszym liczniku.
// Ekran oznaczamy jako brudny tylko przy ZMIANIE wyswietlanej sekundy — inaczej
// przy skanowaniu wolalibysmy o magistrale w kazdej iteracji petli.
static void setPlayPosition(uint32_t sec) {
    uint8_t m = (uint8_t)((sec / 60) % 100);
    uint8_t s = (uint8_t)(sec % 60);
    playBaseMs = millis() - (unsigned long)sec * 1000;
    if (m != playMinutes || s != playSeconds) {
        playMinutes = m;
        playSeconds = s;
        needDisplayUpdate = true;
    }
}

// Ile sekund materialu przeskanowano po `elapsedMs` trzymania klawisza FF/REW.
// Trzy etapy predkosci (Config.h): najpierw wolno — da sie trafic w konkretne
// miejsce — potem coraz szybciej, jak w oryginalnej zmieniarce.
static uint32_t scanDistanceSec(unsigned long elapsedMs) {
    unsigned long t = elapsedMs;
    uint32_t dist = 0;

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

// Dopchnij dekoder do aktualnej pozycji wyswietlacza (jeden setTimeOffset).
static void syncAudioToDisplayClock() {
    uint32_t target = (uint32_t)playMinutes * 60u + (uint32_t)playSeconds;
    audioSeekToSec(target);
    lastAudioSeek = millis();
}

static void endSeekScan() {
    seekScanDir = 0;
    needDisplayUpdate = true;
    // Finalny sync: ekran pokazuje pozycje po skanie, audio moze byc w tyle
    // (aktualizowane tylko co SEEK_AUDIO_MS).
    syncAudioToDisplayClock();
    audioSetInfoSquelch(false);
}

void seek(int deltaSec) {
    // Komenda FF/REW od radia (0x24/0x25) = WCISNIECIE klawisza. Puszczenie
    // przychodzi jako broadcast `08 00` i trafia do stopSeekScan().
    if (cdState != MechState::Playing && cdState != MechState::Seeking) return;
    const int dir = (deltaSec >= 0) ? +1 : -1;
    const unsigned long now = millis();

    // Ten sam kierunek w trakcie skanu = powtorzona ramka od radia. Skan trwa
    // dalej; NIE restartujemy kotwicy, bo zgubiloby to przyspieszenie.
    if (seekScanDir == dir) return;

    // Start skanu (albo zmiana kierunku): kotwica = biezaca pozycja, od niej
    // liczymy przebyty dystans jako funkcje czasu trzymania klawisza.
    seekAnchorSec = (uint32_t)playMinutes * 60 + playSeconds;
    seekScanDir   = dir;
    seekScanStart = now;
    lastAudioSeek = now;   // pierwszy podglad audio dopiero po SEEK_AUDIO_MS

    if (cdState != MechState::Seeking) {
        enterState(MechState::Seeking);
        audioSetInfoSquelch(true);
    }
    needDisplayUpdate = true;
    Serial.printf(">> SCAN %s start: CD%d TR%d %02d:%02d\n",
                  dir > 0 ? "FF" : "REW", currentDisk, currentTrack,
                  playMinutes, playSeconds);
}

void serviceSeekRepeat(unsigned long now) {
    if (seekScanDir == 0) return;

    // Skanowanie tylko w stanach Playing lub Seeking. W innych (LoadingTrack,
    // ChangedCd itp.) automatycznie konczymy.
    if (cdState != MechState::Playing && cdState != MechState::Seeking) {
        endSeekScan();
        return;
    }

    // Bezpiecznik: gdyby radio nie przyslalo `08 00` konczacego przewijanie.
    if (now - seekScanStart > SEEK_SCAN_MAX_MS) {
        Serial.printf(">> SCAN auto-stop (limit %lus): CD%d TR%d %02d:%02d\n",
                      SEEK_SCAN_MAX_MS / 1000, currentDisk, currentTrack,
                      playMinutes, playSeconds);
        endSeekScan();
        return;
    }

    // Pozycja liczona CIAGLE z czasu trzymania klawisza — licznik plynie gladko
    // i przyspiesza, zamiast skakac co stala porcje sekund.
    long target = (long)seekAnchorSec +
                  (long)seekScanDir * (long)scanDistanceSec(now - seekScanStart);
    if (target < 0) target = 0;
    const uint32_t dur = audioGetDurationSec();
    if (dur > 1 && target > (long)(dur - 1)) target = (long)(dur - 1);
    setPlayPosition((uint32_t)target);

    // Slyszalne cue: co SEEK_AUDIO_MS dekoder wskakuje w biezaca pozycje.
    if (now - lastAudioSeek >= SEEK_AUDIO_MS) {
        syncAudioToDisplayClock();
    }
}

// --- ZATRZYMANIE SKANOWANIA: PUSZCZENIE KLAWISZA (broadcast `18 10 08 00`) ---
// Potwierdzone logami: `08 00` przychodzi dokladnie tyle po ramce 0x24/0x25, ile
// trwalo przytrzymanie klawisza (np. FF 21:07:00.3 -> `08 00` 21:07:03.6).
void stopSeekScan() {
    if (seekScanDir != 0) {
        Serial.printf(">> SCAN stop (0x08 0x00): CD%d TR%d %02d:%02d\n",
                      currentDisk, currentTrack, playMinutes, playSeconds);
        endSeekScan();
    }
}

// --- BEZPOŚREDNI WYBÓR PŁYTY/UTWORU (0xB0) ---
void selectDiscTrack(uint8_t disc, uint8_t track) {
    currentDisk  = disc;
    currentTrack = track;
    enterSeek();
    needDisplayUpdate = true;
}

void resetToInit() {
    enterState(MechState::Init);
    initWaitTime = 0;
}

void resetToAllocated() {
    // Lekki reset po SYSTEM RESET radia: zachowaj stan odtwarzania jezeli audio
    // nadal gra. Prawdziwe radio podczas SYSTEM RESET nie zatrzymuje zmieniarki
    // — po re-discovery zmieniarka wraca z zachowanym stanem (sniff potwierdza:
    // po resecie zmieniarka odpowiada aktualnym czasem, nie zerowym).
    if (audioIsPlaying()) {
        // Audio gra — zachowaj stan Playing i zsynchronizuj czas z pozycja dekodera.
        uint32_t t = audioGetCurrentTimeSec();
        playMinutes = (uint8_t)((t / 60) % 100);
        playSeconds = (uint8_t)(t % 60);
        playBaseMs = millis() - (unsigned long)t * 1000;
        // Upewnij sie ze jestesmy w stanie Playing (moze byc Init/Idle po starym resecie)
        if (cdState != MechState::Playing) {
            enterState(MechState::Playing);
        }
        needDisplayUpdate = true;
        Serial.printf(">> resetToAllocated: audio gra, zachowuje Playing %02d:%02d\n",
                      playMinutes, playSeconds);
    } else {
        // Audio nie gra — normalny reset do Init.
        resetToInit();
    }
}

// --- TRYBY REPEAT / SHUFFLE / INTRO (Wymaganie 8) ---
// Zmiana trybu oznacza zakolejkowanie ramki 0x94.
// Te metody sa wywolywane przez UnilinkProtocol (handlePacket) przy
// komendach 0x34/0x35/0x36 — po kazdej zmianie (wlacznie z auto-advance)
// zakolejkowana jest ramka 0x94 przez CdChanger::enqueueModeIcons().
void toggleRepeat() {
    // Cykl Off -> One -> All -> Off (R8.1).
    switch (playModesState.repeat) {
        case RepeatMode::Off: playModesState.repeat = RepeatMode::One; break;
        case RepeatMode::One: playModesState.repeat = RepeatMode::All; break;
        case RepeatMode::All: playModesState.repeat = RepeatMode::Off; break;
    }
    needDisplayUpdate = true;
    enqueueModeIcons();  // zakolejkuj ramke 0x94
    Serial.printf(">> TOGGLE REPEAT -> %d\n", (int)playModesState.repeat);
}

void toggleShuffle() {
    playModesState.shuffle = !playModesState.shuffle;   // R8.2
    needDisplayUpdate = true;
    enqueueModeIcons();  // zakolejkuj ramke 0x94
    Serial.printf(">> TOGGLE SHUFFLE -> %d\n", playModesState.shuffle ? 1 : 0);
}

void toggleIntro() {
    playModesState.intro = !playModesState.intro;       // R8.3
    needDisplayUpdate = true;
    enqueueModeIcons();  // zakolejkuj ramke 0x94
    Serial.printf(">> TOGGLE INTRO -> %d\n", playModesState.intro ? 1 : 0);
}

void noteFirstPing() {
    if (cdState == MechState::Init && initWaitTime == 0) {
        initWaitTime = millis();
    }
}

void notePolled() {
    if (pollsInState < 0xFFFF) pollsInState++;
}

void sleep() {
    // Radio uspione/wylaczone — zatrzymaj dzwiek i zapamietaj ostatni utwor.
    // Magistrala jest juz wylaczona, wiec blokujacy zapis NVS nikomu nie szkodzi.
    seekScanDir = 0;
    audioSetInfoSquelch(false);
    doPersist();
    audioStop();
    enterState(MechState::Init);
    initWaitTime = 0;
    needDisplayUpdate = false;
}

void servicePersist(unsigned long microsSinceLastClock) {
    // Flush tylko gdy magistrala jest naprawde bezczynna — zapis flash blokuje
    // petle na kilkadziesiat ms, wiec nie moze trafic w aktywna wymiane z radiem.
    if (persistPending && microsSinceLastClock > PERSIST_FLUSH_IDLE_US) {
        doPersist();
    }
}

void wake() {
    // Pobudka — upewnij sie, ze DAC ma docelowa glosnosc.
    audioSetVolume(AUDIO_VOLUME);
}

// --- KOLEJKOWANIE RAMKI IKON 0x94 (R8.4, Wymaganie 8.4) ---
// Wywolywane po kazdej zmianie trybu (toggle* lub selectNextTrackAuto).
// Buduje gotowa ramke 0x94 i zakolekcja ja w TxQueue. Adresowanie: RAD=0x70,
// TAD=myAddr, CMD1=0x94 (middle, 4 bajty danych D1..D4).
// Priorytet: Tx::PRIO_MAGAZINE (jak inne ramki statusowe).
// Zawiera kodowanie stanu trybow w D1 (shuffle bit0, intro bit1, repeat w bity 4-5).
void enqueueModeIcons() {
    // Delegacja do UnilinkProtocol — tylko on ma dostep do globalnego `myAddr`
    // i funkcji `enqueue()`. To jedno zrodlo prawdy o budowie i nadawaniu ramek.
    // W trybie testowym (PBT) ta funkcja nie jest uzywana — PBT testuja
    // UnilinkFrame::encodeIconData/decodeIconData w izolacji (zadanie 11.5).
    UnilinkProtocol::enqueueModeIconsHelper((uint8_t)playModesState.repeat,
                                            playModesState.shuffle,
                                            playModesState.intro);
}

MechState mechState() { return cdState; }

// Bajt statusu §7.1 wysylany w PONG i ramkach statusu. Mapowanie realizuje
// czysta funkcja UnilinkFrame::statusByte (jedno zrodlo prawdy, testowalne na
// hoscie — Property 7 / zadanie 6.3).
uint8_t statusByte() { return UnilinkFrame::statusByte(cdState); }

uint8_t disk()     { return currentDisk; }
uint8_t track()    { return currentTrack; }
uint8_t minutes()  { return playMinutes; }
uint8_t seconds()  { return playSeconds; }

// --- USTAWIANIE STANU (dla protokolu: 0xB0 bezposredni wybor plyty) ---
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

// --- MODELCZYG FUNKCJE DO TESTOW PROPERTY-BASED (Property 16) ---
// Wydziela czysta logike wyboru nastepnego utworu dla testow.
NextTrackResult modelNextTrack(PlayModes modes, uint8_t disc, uint8_t track,
                               uint8_t maxDisc, uint8_t maxTrack) {
    NextTrackResult result = {disc, track};
    
    switch (modes.repeat) {
        case RepeatMode::One:
            // Repeat::One -> powtarzamy ten sam utwór (nie zmieniamy pozycji)
            // Nie wywołujemy enterSeek w serviceAutoAdvance dla Repeat::One
            break;
            
        case RepeatMode::All:
            // Repeat::All -> zawijamy po ostatniej plycie (wracamy do CD1)
            if (track < maxTrack) {
                result.nextTrack = track + 1;
            } else {
                // Ostatni utwor na plycie
                result.nextTrack = 1;
                // Znajdz nastepna niepusta plyte
                uint8_t nextNonEmpty = audioFindNextNonEmptyDisc(disc);
                if (nextNonEmpty != 0) {
                    result.nextDisc = nextNonEmpty;
                } else {
                    // Brak nastepnej plyty - zawijamy do CD1
                    result.nextDisc = (disc >= maxDisc) ? 1 : disc + 1;
                }
            }
            break;
            
        case RepeatMode::Off:
            // Repeat::Off -> zatrzymujemy po ostatnim utworze ostatniej plyty
            if (track < maxTrack) {
                result.nextTrack = track + 1;
            } else {
                // Ostatni utwor na plycie - sprawdzamy czy sa kolejne plyty
                uint8_t nextNonEmpty = audioFindNextNonEmptyDisc(disc);
                if (nextNonEmpty != 0) {
                    result.nextDisc = nextNonEmpty;
                    result.nextTrack = 1;
                }
                // W przeciwnym razie pozostajemy na ostatnim utworze (brak zawijania)
            }
            break;
    }
    
    return result;
}

void setSeconds(uint8_t sec) { playSeconds = sec; }
void setMinutes(uint8_t min) { playMinutes = min; }

} // namespace CdChanger
