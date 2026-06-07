#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <Arduino.h>

// --- KONFIGURACJA PINÓW I2S (PCM5102A DAC) ---
// PCM5102A SCK → podłączyć do GND (moduł sam generuje zegar z BCK/LRCK)
#define I2S_BCK_PIN    7   // Bit Clock
#define I2S_LRCK_PIN  15   // Word Select (Left/Right Clock)
#define I2S_DIN_PIN   13   // Data In
// GPIO 12 — wolny (zapasowy kabel)

// --- NOŚNIK: USB PENDRIVE ---
// Pendrive podłączany do portu USB-OTG (GPIO 19/20, obsługiwane przez UsbDrive.h)
// Nie potrzebne dodatkowe piny — USB OTG jest wbudowane w ESP32-S3

// --- STAŁE ---
#define MAX_DISCS       10
#define MAX_TRACKS      99
#define AUDIO_VOLUME    18   // 0..21 (biblioteka ESP32-audioI2S)

// --- INTERFEJS MODUŁU AUDIO ---

// Inicjalizacja USB Host i I2S. Skanuje foldery CD01..CD10 na pendrivie.
// Zwraca true jeśli inicjalizacja powiodła się.
bool audioInit();

// Rozpocznij odtwarzanie tracka (disc: 1..10, track: 1..99).
// Jeśli plik nie istnieje — próbuje .mp3, potem .wav, .flac, .aac.
// Zwraca true jeśli plik znaleziony i odtwarzanie rozpoczęte.
bool audioPlayTrack(uint8_t disc, uint8_t track);

// Zatrzymaj odtwarzanie.
void audioStop();

// Czy aktualnie odtwarza muzykę?
bool audioIsPlaying();

// Pobierz aktualny czas odtwarzania (sekundy od początku utworu).
uint32_t audioGetCurrentTimeSec();

// Pobierz całkowity czas trwania utworu (sekundy). 0 jeśli nieznany.
uint32_t audioGetDurationSec();

// Ile tracków ma dany dysk (1..10). 0 = dysk pusty/brak folderu.
uint8_t audioGetTrackCount(uint8_t disc);

// Znajdź następny niepusty dysk (startując od disc+1, zawijając do 1..10).
// Zwraca 0 jeśli ŻADEN dysk nie ma tracków.
uint8_t audioFindNextNonEmptyDisc(uint8_t disc);

// Znajdź poprzedni niepusty dysk.
uint8_t audioFindPrevNonEmptyDisc(uint8_t disc);

// Czy piosenka właśnie się skończyła? (jednorazowa flaga, resetuje się po odczycie)
bool audioSongFinished();

// MUSI być wywoływane w każdej iteracji loop()!
// Obsługuje dekodowanie audio, przesyłanie I2S, i hot-plug pendrive'a.
void audioLoop();

// Ponowne skanowanie folderów (np. po podpięciu pendrive'a).
void audioRescan();

// Ustaw głośność (0..21).
void audioSetVolume(uint8_t vol);

#endif // AUDIO_PLAYER_H
