#ifndef CD_CHANGER_H
#define CD_CHANGER_H

#include <Arduino.h>
#include "UnilinkFrame.h"

// =============================================================================
// CdChanger - model wirtualnej 10-plytowej zmieniarki CD
// =============================================================================
// Trzyma stan odtwarzania (plyta / utwor / czas), maszyne stanow mechanizmu,
// pamiec nieulotna ostatniego utworu (NVS) oraz koordynuje rzeczywiste
// odtwarzanie plikow przez modul AudioPlayer. Nie wie nic o magistrali UniLink
// ani o ramkach - udostepnia czysty interfejs "co i jak gra".
// =============================================================================

namespace CdChanger {

// Wewnetrzny stan mechanizmu zmieniarki - NIEZALEZNY od bajtu statusu UniLink.
// Wczesniej `enum State` mieszal stan mechanizmu z bajtem statusu PONG (np.
// STATE_INIT = 0xC0, co w Kompendium 7.1 oznacza Ejecting), powodujac kolizje
// semantyczne. Definicja typu zyje w czystym module UnilinkFrame (bez Arduino),
// dzieki czemu mapowanie stanu na bajt statusu (UnilinkFrame::statusByte) jest
// w pelni testowalne na hoscie. Tu uzywamy go przez alias.
using MechState = UnilinkFrame::MechState;

// --- TRYBY ODTWARZANIA (Repeat / Shuffle / Intro) - Wymaganie 8 ---
// Repeat ma trzy stany cyklowane komenda 0x34: Off -> One -> All -> Off.
// Shuffle (0x35) i Intro (0x36) sa zwyklymi przelacznikami bool.
enum class RepeatMode : uint8_t { Off, One, All };
struct PlayModes {
    RepeatMode repeat;
    bool shuffle;
    bool intro;
};

// ============================================================
// Helpers do budowania i dekodowania ramki ikon 0x94 (R8.4)
// ============================================================
// Wymienna para encode/decode do round-trip stan <-> ramka 0x94.
// Umieszczone tutaj, by byly testowalne w jednostkach i PBT (zadania 11.2, 11.5).

// Zbuduj i zakolejkuj ramke ikon 0x94 (middle, 11B, RAD=0x70, TAD=myAddr).
// Wywolywane po kazdej zmianie trybu (toggleRepeat/Shuffle/Intro lub auto-advance).
// Priorytet: Tx::PRIO_MAGAZINE (jak inne ramki statusowe).
// Dla PBT (zadanie 11.5) ta funkcja jest uzywana przez UnilinkProtocol,
// ktory ma dostep do CdChanger::playModes().
void enqueueModeIcons();

// Inicjalizacja: NVS (wczytanie ostatniego utworu). Wywolac w setup().
void begin();

// Maszyna stanow + sekundnik. Wywolywac w kazdej iteracji loop().
// `radioEngaged` = radio przydzielilo nam adres (potrzebne do przejscia
// INIT -> IDLE, ktore modeluje rozgrzanie mechanizmu po nawiazaniu sesji).
void update(unsigned long now, bool radioEngaged);

// Bezpieczny, odroczony zapis NVS. Wywolywac w loop() z czasem ciszy magistrali
// (mikrosekundy od ostatniego zbocza zegara). Zapis wykona sie tylko gdy
// magistrala jest bezczynna, by nie blokowac odpowiedzi na radio.
void servicePersist(unsigned long microsSinceLastClock);

// Obsluga zdarzen z modulu audio (auto-next po koncu utworu) oraz wykrycia
// nosnika USB (wznowienie zapamietanej plyty). Wywolywac w kazdej iteracji.
void serviceAutoAdvance();
void serviceMediaMount();

// Skanowanie przewijaniem (FF/REW). Wywolywac w kazdej iteracji loop().
// Po nacisnieciu FF/REW aktualizuje ekran co SEEK_REPEAT_MS i dekoder MP3
// co SEEK_AUDIO_MS (podglad), az do ponownego nacisniecia / 0x08 / limitu.
void serviceSeekRepeat(unsigned long now);

// Zatrzymaj skanowanie przewijania (wywolywane przy odbiorze broadcastu 0x08).
// Ustawia seekScanDir=0, konczy skanowanie zatrzaskowe.
void stopSeekScan();

// --- KOMENDY OD RADIA / PANELU ---
void handlePlayCommand();   // 0x20 0x00: wznow lub rozpocznij odtwarzanie
void nextTrack();
void prevTrack();
void nextDisc();
void prevDisc();
void seek(int deltaSec);   // 0x24/0x25: przewijanie w obrebie utworu (FF/REW)
void selectDiscTrack(uint8_t disc, uint8_t track);  // 0xB0: bezposredni wybor plyty/utworu

// --- TRYBY REPEAT / SHUFFLE / INTRO (Wymaganie 8) ---
// Zmiana trybu oznacza zakolejkowanie ramki 0x94. Te metody wywolywane sa
// przez UnilinkProtocol (handlePacket) przy komendach 0x34/0x35/0x36.
// Po kazdej zmianie (wlacznie z auto-advance) zakolejkowana jest ramka 0x94.
void toggleRepeat();        // 0x34: cykl Repeat Off->One->All->Off
void toggleShuffle();       // 0x35: przelacznik Shuffle
void toggleIntro();         // 0x36: przelacznik Intro

// --- NAWIGACJA Z UWZGLEDNIENIEM TRYBU REPEAT (Wymaganie 8.5) ---
// Wybor nastepnego utworu wg trybu Repeat. Zwraca true, jezeli nastapila
// zmiana utworu/plyty (entersSeek), false jezeli odtwarzanie sie zatrzymalo.
//   Repeat::One  -> powtarza ten sam utwór (zwraca false, brak zmiany)
//   Repeat::All  -> zawija po ostatniej plycie (wraca do CD1)
//   Repeat::Off  -> zatrzymuje po ostatnim utworze ostatniej plyty (brak zmiany)
bool selectNextTrackAuto();

// --- ZARZADZANIE STANEM SESJI (wywolywane przez protokol) ---
void resetToInit();         // ustaw stan INIT (po appoint/reset radia)
// Lekki reset po SYSTEM RESET gdy audio nadal gra: zachowaj stan Playing
// i zsynchronizuj czas z aktualna pozycja audio (zamiast resetowac do Init).
// Uzywane zamiast resetToInit() w handlerze 01 00 i auto-recovery 01 11.
void resetToAllocated();
void noteFirstPing();       // pierwszy PING w stanie INIT startuje licznik 0xC0->0x80
// Radio wlasnie odpytalo nas o status (`01 12`) albo pobralo ekran (`01 13`).
// Maszyna stanow mechanizmu opuszcza stan przejsciowy dopiero, gdy radio zdazylo
// go zobaczyc — inaczej wirtualny mechanizm przeskakiwalby etapy niezauwazenie.
void notePolled();
void sleep();               // BUS=0 / timeout: zatrzymaj audio + zapisz NVS
void wake();                // BUS=1: przywroc docelowa glosnosc

// --- DOSTEP DO STANU (dla protokolu) ---
MechState mechState();      // wewnetrzny stan mechanizmu (zrodlo prawdy)
uint8_t statusByte();       // bajt statusu 7.1 = UnilinkFrame::statusByte(mechState())
uint8_t disk();
uint8_t track();
uint8_t minutes();
uint8_t seconds();

// Tryby odtwarzania (Wymaganie 8). `playModes()` zwraca caly stan; pomocnicze
// akcesory dla wygody (np. budowa ramki ikon 0x94 / nawigacja repeat-aware).
PlayModes  playModes();
RepeatMode repeatMode();
bool       shuffle();
bool       intro();

// --- USTAWIANIE STANU (dla protokolu: 0xB0 bezposredni wybor plyty) ---
void setDisk(uint8_t disc);
void setTrack(uint8_t track);

// Flaga "ekran wymaga aktualizacji".
bool isDisplayDirty();
void clearDisplayDirty();

// --- WYBOR NASTEPCZEGO UTWORU Z UWZGLEDNIENIEM TRYBU REPEAT (Wymaganie 8.5) ---
// WYNIK modelNextTrack - wyznacza nastepna pozycje wg trybu Repeat.
struct NextTrackResult {
    uint8_t nextDisc;
    uint8_t nextTrack;
};

// Modelowa funkcja wyboru nastepnego utworu wg trybu Repeat.
//   Repeat::One  -> powtarza ten sam utwor (nextDisc=currentDisk, nextTrack=currentTrack)
//   Repeat::All  -> zawija po ostatniej plycie (wraca do CD1)
//   Repeat::Off  -> zatrzymuje po ostatnim utworze ostatniej plyty (nextDisc=0, nextTrack=0)
// Zwraca NextTrackResult z nowa pozycja. Uzywane przez serviceAutoAdvance (zadanie 11.6).
NextTrackResult modelNextTrack(PlayModes modes, uint8_t disc, uint8_t track,
                               uint8_t maxDisc, uint8_t maxTrack);

// Ustawianie licznika czasu - dla harmonogramu 1Hz w protokole.
// Wywolywane przez UnilinkProtocol::servicePositionFrame1Hz.
void setSeconds(uint8_t sec);
void setMinutes(uint8_t min);

} // namespace CdChanger

#endif // CD_CHANGER_H