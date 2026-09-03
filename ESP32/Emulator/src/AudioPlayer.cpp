#include "AudioPlayer.h"
#include "UsbDrive.h"
#include "Config.h"
#include "Audio.h"     // ESP32-audioI2S by schreibfaul1
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

// --- Obiekt audio (globalny singleton z biblioteki ESP32-audioI2S) ---
Audio audio;

// --- Stan wewnętrzny ---
// Przechowujemy posortowane nazwy plików dla każdego dysku.
// trackFiles[disc][index] = nazwa pliku (basename, np. "Metallica - Enter Sandman.mp3")
// trackCount[disc] = ile plików znaleziono na danym dysku
// Indeksy: disc 1..10, track index 0-based wewnętrznie (externally 1-based)
#define MAX_TRACKS_STORED  MAX_TRACKS   // max 99 tracków na dysk
static String trackFiles[MAX_DISCS + 1][MAX_TRACKS_STORED]; // [disc 1..10][0..98]
static uint8_t trackCount[MAX_DISCS + 1]; // indeks 1..10, [0] nieużywany

// Katalog płyty na nośniku i etykieta pokazywana w radiu.
// Konwencja nazw folderów: "CDnn" albo "CDnn.Nazwa" (np. "CD01.ABBA").
// Numer slotu zostaje w nazwie katalogu (zmieniarka wie, który folder to która
// płyta), a na ekran radia idzie wyłącznie część po kropce ("ABBA").
static String discDirName[MAX_DISCS + 1];  // np. "CD01.ABBA"
static String discLabel[MAX_DISCS + 1];    // np. "ABBA"
static volatile bool songFinishedFlag = false;
static bool isPlaying = false;
static bool wasUsbMounted = false;   // do wykrywania hot-plug/unplug

// Flaga "trwa zmiana utworu" — chroni przed FALSZYWYM wykryciem konca utworu.
// Gdy zmieniamy track/plyte, audio.stopSong() na chwile ustawia isRunning()=false,
// a getAudioCurrentTime() zwraca jeszcze stary czas (>0). Bez tej flagi
// awaryjny detektor konca utworu odpalal AUTO-NEXT i przeskakiwal na TR2.
static volatile bool trackChanging = false;

// Flaga "utwor faktycznie ruszyl" — ustawiana gdy biblioteka zacznie dekodowac
// (audio.isRunning()==true). Kasowana przy zadaniu nowego utworu. Dzieki niej
// wykrywamy KONIEC utworu nawet gdy biblioteka zeruje getAudioCurrentTime() do 0
// (tak dzieje sie przy naturalnym EOF). Wczesniej detektor zapasowy wymagal
// getAudioCurrentTime() > 0, przez co po skonczeniu utworu (czas=0, isRunning=0)
// nigdy nie odpalal AUTO-NEXT i zegar tykal w nieskonczonosc.
static volatile bool songStarted = false;

// --- Synchronizacja audio (osobny task FreeRTOS) ---
static SemaphoreHandle_t audioMutex = NULL;
static TaskHandle_t audioTaskHandle = NULL;

// --- Żądania asynchroniczne ---
static volatile bool playRequestPending = false;
static char playRequestPath[128];

// Sterowanie audio z GLOWNEJ petli (Core 1) NIGDY nie moze blokowac sie na
// audioMutex — w przeciwnym razie nacisniecie klawisza czeka, az task audio
// zwolni mutex (np. po connecttoFS/decode), co zamraza odpowiedzi na radio,
// licznik czasu i gubi powtorzone komendy FF/REW. Dlatego seek/stop/volume
// przekazujemy jako ZADANIA, ktore realizuje task audio (Core 0).
static volatile bool seekRequestPending = false;
static volatile int  seekRequestDelta   = 0;   // akumulowany skok (+/- s)
static volatile bool stopRequestPending = false;
static volatile bool volRequestPending  = false;
static volatile uint8_t volRequestValue = AUDIO_VOLUME;
// Gdy true — audio_info pomija wysokoczęstotliwosciowe komunikaty dekodera.
// Ustawiane z CdChanger na czas skanowania FF/REW (patrz audioSetInfoSquelch).
static volatile bool infoSquelch = false;


// ============================================================
// Pomocnicza: czy plik ma obsługiwane rozszerzenie audio?
// ============================================================
static bool isAudioFile(const String &name) {
    int dotPos = name.lastIndexOf('.');
    if (dotPos < 1) return false;
    
    String ext = name.substring(dotPos);
    ext.toLowerCase();
    return (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".aac");
}

// ============================================================
// Pomocnicza: wyciągnij basename ze ścieżki
// ESP32 VFS: File::name() zwraca pełną ścieżkę (np. "/usb/CD01/plik.mp3")
// ============================================================
static String getBasename(const String &path) {
    int lastSlash = path.lastIndexOf('/');
    return (lastSlash >= 0) ? path.substring(lastSlash + 1) : path;
}

// ============================================================
// Proste sortowanie alfabetyczne (insertion sort — wystarczy dla <100 plików)
// ============================================================
static void sortStrings(String arr[], int count) {
    for (int i = 1; i < count; i++) {
        String key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

// ============================================================
// Rozpoznanie katalogu płyty: "CDnn" albo "CDnn.Nazwa"
// Zwraca numer płyty 1..MAX_DISCS, albo 0 gdy nazwa nie pasuje.
// `label` dostaje część po kropce, a gdy jej nie ma — samo "CDnn".
// ============================================================
static uint8_t parseDiscDir(const String &dirName, String &label) {
    if (dirName.length() < 4) return 0;

    const char c0 = dirName[0];
    const char c1 = dirName[1];
    if ((c0 != 'C' && c0 != 'c') || (c1 != 'D' && c1 != 'd')) return 0;
    if (dirName[2] < '0' || dirName[2] > '9') return 0;
    if (dirName[3] < '0' || dirName[3] > '9') return 0;

    const int num = (dirName[2] - '0') * 10 + (dirName[3] - '0');
    if (num < 1 || num > MAX_DISCS) return 0;

    // Sam numer, bez sufiksu — etykietą zostaje nazwa folderu.
    if (dirName.length() == 4) {
        label = dirName;
        return (uint8_t)num;
    }

    // Po numerze dopuszczamy WYŁĄCZNIE kropkę, żeby np. "CD01backup" nie
    // przechwycił slotu 1 przypadkiem.
    if (dirName[4] != '.') return 0;

    String rest = dirName.substring(5);
    rest.trim();
    label = (rest.length() > 0) ? rest : dirName.substring(0, 4);
    return (uint8_t)num;
}

// ============================================================
// Skanowanie folderów płyt na pendrivie
// Zbiera WSZYSTKIE pliki audio (dowolna nazwa), sortuje
// alfabetycznie i przypisuje numery tracków 1, 2, 3...
// ============================================================
static void scanDiscs() {
    if (!usbDriveIsMounted()) {
        Serial.println("[Audio] Pendrive nie zamontowany — nie mogę skanować.");
        return;
    }

    fs::FS &fs = usbDriveGetFS();

    for (int d = 1; d <= MAX_DISCS; d++) {
        trackCount[d]  = 0;
        discDirName[d] = "";
        discLabel[d]   = "";
        // Wyczyść stare wpisy
        for (int t = 0; t < MAX_TRACKS_STORED; t++) {
            trackFiles[d][t] = "";
        }
    }

    // --- Krok 1: przypisz katalogi z nośnika do slotów płyt ---
    // Katalogu nie da się już otworzyć "po numerze", bo nazwa może mieć dowolny
    // sufiks po kropce — trzeba przejrzeć katalog główny.
    File root = fs.open("/");
    if (root && root.isDirectory()) {
        File entry;
        while ((entry = root.openNextFile())) {
            if (entry.isDirectory()) {
                const String name = getBasename(String(entry.name()));
                String label;
                const uint8_t d = parseDiscDir(name, label);
                // Przy dwóch folderach na ten sam numer wygrywa pierwszy.
                if (d != 0 && discDirName[d].length() == 0) {
                    discDirName[d] = name;
                    discLabel[d]   = label;
                }
            }
            entry.close();
        }
    }
    if (root) root.close();

    // --- Krok 2: zbierz pliki audio z każdego znalezionego katalogu ---
    for (int d = 1; d <= MAX_DISCS; d++) {
        if (discDirName[d].length() == 0) continue;

        const String dirPath = String("/") + discDirName[d];

        File dir = fs.open(dirPath.c_str());
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            continue;
        }
        
        // Zbierz wszystkie pliki audio z folderu
        int count = 0;
        File entry;
        while ((entry = dir.openNextFile()) && count < MAX_TRACKS_STORED) {
            if (entry.isDirectory()) {
                entry.close();
                continue;
            }
            
            String fullPath = entry.name();
            entry.close();
            
            String name = getBasename(fullPath);
            
            if (isAudioFile(name)) {
                trackFiles[d][count] = name;
                count++;
            }
        }
        dir.close();
        
        if (count == 0) continue;
        
        // Sortuj alfabetycznie — kolejność plików = numery tracków
        sortStrings(trackFiles[d], count);
        trackCount[d] = count;
        
        Serial.printf("[Audio] CD%02d [%s] nazwa=\"%s\": %d track(ów)\n",
                      d, discDirName[d].c_str(), discLabel[d].c_str(), count);
        for (int t = 0; t < count; t++) {
            Serial.printf("[Audio]   TR%02d: %s\n", t + 1, trackFiles[d][t].c_str());
        }
    }
}

// ============================================================
// Szukanie pliku: mapuje (disc, track) na ścieżkę
// track jest 1-based (jak na radiu)
// ============================================================
static bool findTrackPath(uint8_t disc, uint8_t track, char* outPath, size_t pathLen) {
    if (!usbDriveIsMounted()) return false;
    if (disc < 1 || disc > MAX_DISCS) return false;
    if (track < 1 || track > trackCount[disc]) return false;
    
    // track jest 1-based, tablica jest 0-based
    const String &filename = trackFiles[disc][track - 1];
    if (filename.length() == 0) return false;
    if (discDirName[disc].length() == 0) return false;

    snprintf(outPath, pathLen, "/%s/%s", discDirName[disc].c_str(), filename.c_str());
    return true;
}

// ============================================================
// Task dekodowania audio (FreeRTOS)
// ============================================================
static void audioTaskFunc(void *param) {
    Serial.printf("[Audio] Task dekodowania uruchomiony na rdzeniu %d\n", xPortGetCoreID());
    uint32_t loopCounter = 0;
    unsigned long lastDebugTime = 0;
    
    for (;;) {
        // --- Żądania sterujace z Core 1 (nieblokujace dla glownej petli) ---
        if (stopRequestPending) {
            stopRequestPending = false;
            if (audioMutex) xSemaphoreTake(audioMutex, portMAX_DELAY);
            audio.stopSong();
            if (audioMutex) xSemaphoreGive(audioMutex);
            isPlaying = false;
            songStarted = false;
        }
        if (volRequestPending) {
            volRequestPending = false;
            uint8_t v = volRequestValue;
            if (audioMutex) xSemaphoreTake(audioMutex, portMAX_DELAY);
            audio.setVolume(v);
            if (audioMutex) xSemaphoreGive(audioMutex);
        }
        if (seekRequestPending) {
            seekRequestPending = false;
            int d = seekRequestDelta;
            seekRequestDelta = 0;
            if (d != 0 && audio.isRunning()) {
                if (audioMutex) xSemaphoreTake(audioMutex, portMAX_DELAY);
                audio.setTimeOffset(d);
                if (audioMutex) xSemaphoreGive(audioMutex);
            }
        }

        // --- Obsługa żądania nowej piosenki ---
        if (playRequestPending) {
            playRequestPending = false;
            
            if (audioMutex) xSemaphoreTake(audioMutex, portMAX_DELAY);
            audio.stopSong();
            bool result = audio.connecttoFS(usbDriveGetFS(), playRequestPath);
            if (audioMutex) xSemaphoreGive(audioMutex);
            
            Serial.printf("[Audio] Async connecttoFS(\"%s\") = %s\n", playRequestPath, result ? "OK" : "FAIL");
            
            if (!result) {
                isPlaying = false;
                songFinishedFlag = true; // Pomiń do następnego utworu
            } else {
                isPlaying = true;
                Serial.printf("[Audio] ▶ Odtwarzam: %s (rozpoczęto dekodowanie)\n", playRequestPath);
            }
            // Zmiana utworu zakonczona — mozna znow wykrywac koniec utworu.
            trackChanging = false;
        }

        if (audioMutex && xSemaphoreTake(audioMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            audio.loop();
            
            // Diagnostyka co 1 sekundę
            if (isPlaying && millis() - lastDebugTime > 1000) {
                if (DEBUG_VERBOSE) {
                    uint32_t size = audio.getFileSize();
                    uint32_t time = audio.getAudioCurrentTime();
                    bool running = audio.isRunning();
                    Serial.printf("[Audio-Diag] FileSize: %lu bytes | Czas: %lu s | isRunning: %d\n", 
                                  (unsigned long)size, (unsigned long)time, running);
                }
                lastDebugTime = millis();
            }
            
            xSemaphoreGive(audioMutex);
        }
        
        loopCounter++;
        if (loopCounter % 10000 == 0 && isPlaying) {
            // Diagnostyka co jakiś czas: czy task w ogóle kręci
            // Serial.printf("[Audio-Task] loop() żyje, isRunning=%d\n", audio.isRunning());
        }

        if (isPlaying) {
            // Kiedy gra, oddaj czas procesora innym taskom o tym samym priorytecie
            // ale nie opóźniaj na sztywno o 1ms, bo dekoder może potrzebować
            // więcej cykli do napełnienia bufora.
            vTaskDelay(1); // Zostawiamy 1ms na USB task
        } else {
            vTaskDelay(10); // Gdy nie gra, oszczędzaj CPU
        }
    }
}

// ============================================================
// Publiczne API
// ============================================================

bool audioInit() {
    Serial.println("[Audio] Inicjalizacja...");
    
    // Zeruj tablicę tracków
    for (int i = 0; i <= MAX_DISCS; i++) trackCount[i] = 0;
    
    // Inicjalizacja USB Host (pendrive)
    if (!usbDriveInit()) {
        Serial.println("[Audio] BŁĄD: USB Host nie uruchomiony!");
        // Kontynuujemy — I2S i tak skonfigurujemy, pendrive może być podpięty później
    }
    
    // Konfiguracja I2S → PCM5102A
    bool pinoutOk = audio.setPinout(I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DIN_PIN);
    Serial.printf("[Audio] setPinout(BCK=%d, LRCK=%d, DIN=%d) = %s\n", 
                  I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DIN_PIN, pinoutOk ? "OK" : "FAIL");
    audio.setVolume(AUDIO_VOLUME);
    
    // Mutex do synchronizacji dostępu do obiektu audio między taskami
    audioMutex = xSemaphoreCreateMutex();
    if (!audioMutex) {
        Serial.println("[Audio] BŁĄD: Nie mogę utworzyć mutexu audio!");
        return false;
    }
    
    // Task dekodowania audio — Core 0 (razem z USB Host), priorytet 2.
    // KLUCZOWE: trzymamy dekoder z DALA od Core 1, na ktorym dziala petla
    // Arduino + przerwanie zegara magistrali Unilink. Wczesniej dekoder (Core 1,
    // prio 2) wywlaszczal petle (prio 1) na 10-20ms przy napelnianiu bufora, co
    // powodowalo spoznione odpowiedzi na radio, gubione klawisze i nierowne
    // odliczanie czasu. Teraz Core 1 nalezy w calosci do warstwy czasu
    // rzeczywistego (Unilink), a Core 0 do "mediow" (USB + dekoder audio).
    BaseType_t ret = xTaskCreatePinnedToCore(
        audioTaskFunc, "audio_dec", 8192, NULL, 2, &audioTaskHandle, 0);
    if (ret != pdPASS) {
        Serial.println("[Audio] BŁĄD: Nie mogę uruchomić tasku audio!");
        return false;
    }
    
    // Jeśli pendrive jest już zamontowany — skanuj foldery
    if (usbDriveIsMounted()) {
        wasUsbMounted = true;
        scanDiscs();
        Serial.println("[Audio] Inicjalizacja zakończona — pendrive gotowy.");
    } else {
        Serial.println("[Audio] Inicjalizacja I2S zakończona — oczekuję na pendrive.");
    }
    
    return true;
}

bool audioPlayTrack(uint8_t disc, uint8_t track) {
    if (!usbDriveIsMounted()) {
        Serial.println("[Audio] Pendrive nie zamontowany — nie mogę odtwarzać.");
        return false;
    }
    
    if (disc < 1 || disc > MAX_DISCS || track < 1 || track > MAX_TRACKS) {
        Serial.printf("[Audio] Nieprawidłowy disc=%d track=%d\n", disc, track);
        return false;
    }
    
    char path[128];  // dłuższy bufor na dowolne nazwy plików
    if (!findTrackPath(disc, track, path, sizeof(path))) {
        Serial.printf("[Audio] Brak pliku: CD%02d track %d (maks=%d)\n", disc, track, trackCount[disc]);
        isPlaying = false;
        return false;
    }
    
    // Zapisz ścieżkę do zmiennej globalnej i podnieś flagę dla taska asynchronicznego
    // Zdejmujemy tym samym WSZELKIE operacje plikowe z głównego wątku (Core 0),
    // by całkowicie uniknąć blokowania przerwań i timeoutów radia (co powodowało System Reset).
    strlcpy(playRequestPath, path, sizeof(playRequestPath));
    songFinishedFlag = false;
    trackChanging = true;     // blokuj awaryjny detektor konca utworu
    songStarted = false;      // nowy utwor jeszcze nie ruszyl
    playRequestPending = true;
    
    return true; // Sukces logistyczny (fizyczne otwarcie w tle)
}

void audioStop() {
    // Nieblokujaco: zlec zatrzymanie taskowi audio. isPlaying gasimy od razu,
    // by reszta systemu natychmiast widziala "nie gram".
    isPlaying = false;
    stopRequestPending = true;
    Serial.println("[Audio] ⏹ Zatrzymano (zlecone).");
}

bool audioIsPlaying() {
    return isPlaying && audio.isRunning();
}

uint32_t audioGetCurrentTimeSec() {
    if (!audioIsPlaying()) return 0;
    return audio.getAudioCurrentTime();
}

uint32_t audioGetDurationSec() {
    return audio.getAudioFileDuration();
}

bool audioSeekRelative(int deltaSec) {
    if (!audioIsPlaying()) return false;
    // Nieblokujaco: akumulujemy skok i zlecamy go taskowi audio. Glowna petla
    // (Core 1) nie czeka na audioMutex, wiec radio i licznik czasu nie zwisza.
    seekRequestDelta += deltaSec;
    seekRequestPending = true;
    return true;
}

bool audioSeekToSec(uint32_t targetSec) {
    if (!audioIsPlaying()) return false;
    int audioNow = (int)audioGetCurrentTimeSec();
    int delta = (int)targetSec - audioNow;
    if (delta == 0) return true;
    return audioSeekRelative(delta);
}

void audioSetInfoSquelch(bool squelch) {
    infoSquelch = squelch;
}

uint8_t audioGetTrackCount(uint8_t disc) {
    if (disc < 1 || disc > MAX_DISCS) return 0;
    return trackCount[disc];
}

// ============================================================
// Interfejs nazw (źródło dla modułu CD-TEXT)
// ============================================================

// Pomocnicza: kopiuje napis do bufora, NUL-terminuje, zwraca liczbę znaków.
static size_t copyBounded(const char* src, char* out, size_t maxLen) {
    if (!out || maxLen == 0) return 0;
    size_t n = 0;
    if (src) {
        while (src[n] != '\0' && n < maxLen - 1) {
            out[n] = src[n];
            n++;
        }
    }
    out[n] = '\0';
    return n;
}

size_t audioGetTrackName(uint8_t disc, uint8_t track, char* out, size_t maxLen) {
    if (out && maxLen > 0) out[0] = '\0';
    if (disc < 1 || disc > MAX_DISCS) return 0;
    if (track < 1 || track > trackCount[disc]) return 0;

    // track jest 1-based, tablica 0-based — nazwa pliku to nazwa utworu.
    const String &filename = trackFiles[disc][track - 1];
    if (filename.length() == 0) return 0;

    // Odetnij rozszerzenie (.mp3/.wav/...) — nie jest częścią nazwy utworu.
    // To NIE jest sanityzacja ASCII (ta odbywa się w CdText) — tylko wyodrębnienie
    // właściwej nazwy z nazwy pliku.
    int dotPos = filename.lastIndexOf('.');
    String name = (dotPos > 0) ? filename.substring(0, dotPos) : filename;

    return copyBounded(name.c_str(), out, maxLen);
}

size_t audioGetDiscName(uint8_t disc, char* out, size_t maxLen) {
    if (out && maxLen > 0) out[0] = '\0';
    if (disc < 1 || disc > MAX_DISCS) return 0;

    // Nazwa płyty pochodzi z katalogu na pendrivie, ale pokazujemy tylko część
    // po kropce: folder "CD01.ABBA" daje na ekranie "ABBA". Gdy folder nie ma
    // sufiksu (samo "CD01") albo nośnik nie jest zamontowany — zostaje "CDnn",
    // bo puste pole radio pokazuje jako brak nazwy płyty.
    if (discLabel[disc].length() > 0) {
        return copyBounded(discLabel[disc].c_str(), out, maxLen);
    }

    char fallback[8];
    snprintf(fallback, sizeof(fallback), "CD%02d", disc);
    return copyBounded(fallback, out, maxLen);
}

uint8_t audioFindNextNonEmptyDisc(uint8_t disc) {
    for (int i = 1; i <= MAX_DISCS; i++) {
        uint8_t candidate = ((disc - 1 + i) % MAX_DISCS) + 1;
        if (trackCount[candidate] > 0) return candidate;
    }
    return 0;
}

uint8_t audioFindPrevNonEmptyDisc(uint8_t disc) {
    for (int i = 1; i <= MAX_DISCS; i++) {
        uint8_t candidate = ((disc - 1 - i + MAX_DISCS) % MAX_DISCS) + 1;
        if (trackCount[candidate] > 0) return candidate;
    }
    return 0;
}

bool audioSongFinished() {
    if (songFinishedFlag) {
        songFinishedFlag = false;
        return true;
    }
    return false;
}

void audioLoop() {
    // --- Hot-plug/unplug pendrive'a ---
    bool isMounted = usbDriveIsMounted();
    
    if (isMounted && !wasUsbMounted) {
        // Pendrive właśnie podpięty → skanuj foldery
        Serial.println("[Audio] Pendrive podpięty — skanowanie folderów...");
        scanDiscs();
    }
    else if (!isMounted && wasUsbMounted) {
        // Pendrive odłączony → zatrzymaj audio, wyczyść tracki
        Serial.println("[Audio] Pendrive odłączony — zatrzymuję odtwarzanie.");
        stopRequestPending = true;   // nieblokujaco (task audio wykona stopSong)
        isPlaying = false;
        songFinishedFlag = false;
        for (int i = 1; i <= MAX_DISCS; i++) {
            trackCount[i]  = 0;
            discDirName[i] = "";
            discLabel[i]   = "";
            for (int t = 0; t < MAX_TRACKS_STORED; t++) {
                trackFiles[i][t] = "";
            }
        }
    }
    wasUsbMounted = isMounted;
    
    // --- Dekodowanie audio → PRZENIESIONE na osobny task (audioTaskFunc) ---
    // audio.loop() nie jest tu już wywoływane — działa na tasku "audio_dec"
    // z priorytetem 2, dzięki czemu nie jest blokowane przez obsługę Unilink.
    
    // --- Wykryj koniec piosenki (zabezpieczenie) ---
    // Jeśli isRunning zmieniło się na false PO tym jak utwor faktycznie ruszyl
    // (songStarted), oznacza to koniec utworu — nawet gdy callback eof nie przyszedl.
    // UWAGA: gatujemy to flagami trackChanging i playRequestPending oraz debouncem,
    // bo podczas zmiany utworu audio.stopSong() chwilowo daje isRunning()=false.
    // NIE polegamy juz na getAudioCurrentTime() > 0 — przy naturalnym EOF biblioteka
    // zeruje czas do 0, przez co stary warunek nigdy nie wykrywal konca utworu
    // (zegar tykal w nieskonczonosc, brak AUTO-NEXT).
    static unsigned long notRunningSince = 0;
    if (isPlaying && !trackChanging && !playRequestPending) {
        if (audio.isRunning()) {
            // Utwor faktycznie gra — zapamietaj, ze ruszyl, i wyzeruj licznik ciszy.
            songStarted = true;
            notRunningSince = 0;
        } else if (songStarted) {
            // Utwor wczesniej gral, a teraz isRunning()==false -> to koniec.
            // NIE sprawdzamy juz getAudioCurrentTime() > 0, bo biblioteka przy
            // naturalnym EOF zeruje czas do 0 (widoczne w logach: "Czas: 0 s").
            // Falszywe wykrycia podczas zmiany utworu sa juz odciete flagami
            // trackChanging / playRequestPending powyzej.
            if (notRunningSince == 0) {
                notRunningSince = millis();
            } else if (millis() - notRunningSince > 300) {
                Serial.println("[Audio] Wykryto koniec odtwarzania (isRunning()==false, debounce)");
                isPlaying = false;
                songStarted = false;
                songFinishedFlag = true;
                notRunningSince = 0;
            }
        }
    } else {
        notRunningSince = 0;
    }
}

void audioRescan() {
    if (usbDriveIsMounted()) {
        Serial.println("[Audio] Ponowne skanowanie pendrive'a...");
        scanDiscs();
    }
}

void audioSetVolume(uint8_t vol) {
    if (vol > 21) vol = 21;
    // Nieblokujaco: zlec zmiane glosnosci taskowi audio.
    volRequestValue = vol;
    volRequestPending = true;
    Serial.printf("[Audio] Głośność: %d/21 (zlecone)\n", vol);
}

// ============================================================
// Callbacki biblioteki ESP32-audioI2S
// ============================================================

// Jeśli biblioteka została skompilowana jako C, może szukać symboli C
#ifdef __cplusplus
#endif

void audio_info(const char *info) {
    if (!info) return;
    // Lawina tych komunikatow przy setTimeOffset (seek MP3) i otwarciu pliku
    // blokuje wspoldzielony UART — Core 1 gubi okno odpowiedzi na `01 15`.
    if (infoSquelch) return;
    if (strstr(info, "decode error") ||
        strstr(info, "INVALID_") ||
        strstr(info, "syncword") ||
        strstr(info, "stream ready") ||
        strstr(info, "buffers freed") ||
        strstr(info, "Reading file") ||
        strstr(info, "Content-Length") ||
        strstr(info, "ID3 ") ||
        strstr(info, "Audio-Length") ||
        strstr(info, "Channels:") ||
        strstr(info, "SampleRate") ||
        strstr(info, "BitsPerSample") ||
        strstr(info, "BitRate") ||
        strstr(info, "PSRAM") ||
        strstr(info, "MP3Decoder") ||
        strstr(info, "inputBufferSize") ||
        strstr(info, "free Heap")) {
        return;
    }
    Serial.printf("[Audio-info] %s\n", info);
}

void audio_id3data(const char *info) {
    // Lawina ID3 przy kazdym otwarciu MP3 (Album/Artist/Title/…) blokuje
    // wspoldzielony UART z Core 1 — gubimy odpowiedzi na `01 15` i radio
    // porzuca Request Polling. Tagow nie potrzebujemy na magistrali.
    (void)info;
}

void audio_eof_mp3(const char *info) {
    Serial.printf("[Audio-eof] Koniec utworu: %s\n", info);
    songFinishedFlag = true;
    isPlaying = false;
    songStarted = false;
}

void audio_showstation(const char *info) {
    Serial.printf("[Audio-station] %s\n", info);
}

void audio_showstreamtitle(const char *info) {
    Serial.printf("[Audio-title] %s\n", info);
}

void audio_bitrate(const char *info) {
    Serial.printf("[Audio-bitrate] %s\n", info);
}

void audio_commercial(const char *info) {
    Serial.printf("[Audio-commercial] %s\n", info);
}

void audio_icyurl(const char *info) {
    Serial.printf("[Audio-icyurl] %s\n", info);
}

void audio_lasthost(const char *info) {
    Serial.printf("[Audio-lasthost] %s\n", info);
}

void audio_eof_speech(const char *info) {
    Serial.printf("[Audio-eof-speech] %s\n", info);
}

#ifdef __cplusplus
#endif
