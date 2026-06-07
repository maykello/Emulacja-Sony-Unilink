#ifndef CD_CHANGER_H
#define CD_CHANGER_H

#include <Arduino.h>

// =============================================================================
// CdChanger — model wirtualnej 10-plytowej zmieniarki CD
// =============================================================================
// Trzyma stan odtwarzania (plyta / utwor / czas), maszyne stanow mechanizmu,
// pamiec nieulotna ostatniego utworu (NVS) oraz koordynuje rzeczywiste
// odtwarzanie plikow przez modul AudioPlayer. Nie wie nic o magistrali UniLink
// ani o ramkach — udostepnia czysty interfejs "co i jak gra".
// =============================================================================

namespace CdChanger {

// Stany mechanizmu zgodne z bajtem statusu UniLink (PING 01 12).
enum State : uint8_t {
    STATE_INIT    = 0xC0,  // mechanizm sie budzi
    STATE_IDLE    = 0x80,  // gotowy, stoi
    STATE_LOADING = 0x40,  // ladowanie plyty
    STATE_SEEKING = 0x20,  // szukanie utworu
    STATE_PLAYING = 0x00,  // odtwarzanie
};

// Inicjalizacja: NVS (wczytanie ostatniego utworu). Wywolac w setup().
void begin();

// Maszyna stanow + sekundnik. Wywolywac w kazdej iteracji loop().
// `radioEngaged` = radio przydzielilo nam adres (potrzebne do przejscia
// INIT -> IDLE, ktore modeluje rozgrzanie mechanizmu po nawiazaniu sesji).
void update(unsigned long now, bool radioEngaged);

// Obsluga zdarzen z modulu audio (auto-next po koncu utworu) oraz wykrycia
// nosnika USB (wznowienie zapamietanej plyty). Wywolywac w kazdej iteracji.
void serviceAutoAdvance();
void serviceMediaMount();

// --- KOMENDY OD RADIA / PANELU ---
void handlePlayCommand();   // 0x20 0x00: wznow lub rozpocznij odtwarzanie
void nextTrack();
void prevTrack();
void nextDisc();
void prevDisc();

// --- ZARZADZANIE STANEM SESJI (wywolywane przez protokol) ---
void resetToInit();         // ustaw stan INIT (po appoint/reset radia)
void noteFirstPing();       // pierwszy PING w stanie INIT startuje licznik 0xC0->0x80
void sleep();               // BUS=0 / timeout: zatrzymaj audio + zapisz NVS
void wake();                // BUS=1: przywroc docelowa glosnosc

// --- DOSTEP DO STANU (dla protokolu) ---
State   state();
uint8_t disk();
uint8_t track();
uint8_t minutes();
uint8_t seconds();

// Flaga "ekran wymaga aktualizacji" — sterownik Slave Break (protokol).
bool isDisplayDirty();
void clearDisplayDirty();

} // namespace CdChanger

#endif // CD_CHANGER_H
