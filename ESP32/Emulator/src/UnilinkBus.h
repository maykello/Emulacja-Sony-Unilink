#ifndef UNILINK_BUS_H
#define UNILINK_BUS_H

#include <Arduino.h>

// =============================================================================
// UnilinkBus — warstwa fizyczna magistrali Sony Unilink
// =============================================================================
// Master (radio) generuje zegar dla KAZDEGO bitu, rowniez dla odpowiedzi slave'a.
// Pomiary ze sniffow prawdziwej zmieniarki na CDX-M670:
//   * bit ~125 us, bajty ida jeden za drugim (~1 ms na bajt),
//   * miedzy ramkami ~5 ms ciszy zegara,
//   * po kazdej ramce master taktuje "slot odpowiedzi"; gdy nikt nie odpowiada,
//     czyta w nim pojedynczy bajt 0x00.
// Odpowiedz musi wiec byc uzbrojona ZANIM master zacznie taktowac slot. Spozniona
// odpowiedz nadana w srodek kolejnej ramki mastera niszczy ja i konczy sie
// SYSTEM RESETem radia — dlatego respond() sprawdza okno czasowe.
//
// Przerwanie zegara dziala w IRAM (ESP_INTR_FLAG_IRAM), wiec bity sa odbierane
// i nadawane takze podczas zapisu do flash (NVS/LittleFS), kiedy zwykle zadania
// stoja. Najczestsze odpowiedzi (ping 01 12, zgloszenie na 01 15, ramka na grant
// 01 13) przerwanie wysyla samo z danych przygotowanych przez zadanie protokolu.
//
// Funkcje "zadaniowe" wolac wylacznie z rdzenia 1 (zadanie protokolu / loop).
// =============================================================================

namespace UnilinkBus {

constexpr int FRAME_MAX = 16;

struct Frame {
    uint8_t  data[FRAME_MAX];
    uint8_t  len;
    uint8_t  replyLen;             // > 0: przerwanie juz odpowiedzialo ramka `reply`
    uint8_t  reply[FRAME_MAX];
    uint32_t endUs;                // czas ostatniego zbocza ramki (esp_timer, us)
    uint32_t edgeSeq;              // licznik zboczy w chwili konca ramki
};

// Konfiguruje piny i przerwanie zegara. `notifyTask` jest budzone po kazdej
// odebranej ramce. Zwraca false, gdy nie udalo sie zainstalowac przerwania.
bool begin(TaskHandle_t notifyTask);

// Czy przerwanie dziala w IRAM (odporne na zapisy flash)?
bool isrInIram();

// Kolejna kompletna ramka odebrana przez przerwanie.
bool popFrame(Frame& out);

// Uzbraja odpowiedz na ramke `f`, o ile master nie zaczal jeszcze slotu
// odpowiedzi. Zwraca false, gdy jest za pozno (odpowiedz NIE zostanie nadana).
bool respond(const Frame& f, const uint8_t* bytes, int len);

bool isTransmitting();
void abortTx();

// Straznik nadawania — wolac w kazdej iteracji zadania protokolu.
void service();

// --- SZYBKA SCIEZKA W PRZERWANIU ---
// enabled  : wolno odpowiadac (BUS_ON stabilnie wysoki),
// pingAddr : adres, pod ktorym odpowiadamy na 01 12 (0 = nie odpowiadamy),
// status   : bajt statusu mechanizmu w odpowiedzi na ping,
// myAddr   : przydzielony adres (granty 01 13), 0 gdy brak.
void setFastPath(bool enabled, uint8_t pingAddr, uint8_t status, uint8_t myAddr);

// Ramka, ktora przerwanie wysle na najblizszy grant 01 13. Dopoki jest
// zaladowana, na kazde 18 10 01 15 zglaszamy sie maska `claimMask`.
void loadGrant(const uint8_t* bytes, int len, uint8_t claimMask);
void clearGrant();
bool grantLoaded();
uint32_t grantsServed();
uint32_t claimsSent();
uint32_t lastClaimUs();

// --- SLAVE BREAK ---
// Gdy magistrala jest bezczynna, master trzyma na DATA fale ~8 ms poziomu
// dominujacego / ~8 ms recesywnego. Slave zglasza chec nadawania, sciagajac
// linie na ~3 ms w fazie recesywnej, ~2 ms po jej poczatku. Master odpowiada
// wtedy `18 10 01 15`. Funkcja czeka na fale aktywnie (ograniczony czas) i
// natychmiast rezygnuje, gdy na zegarze pojawi sie ruch.
enum class BreakResult : uint8_t { Done, Busy, NoIdleWave };
BreakResult trySlaveBreak();

uint32_t nowUs();
uint32_t usSinceLastEdge();
uint32_t edgeCount();
bool busOnRaw();

struct Stats {
    uint32_t frames;
    uint32_t glitches;     // zbocza odrzucone jako zaklocenia
    uint32_t broken;       // niekompletne ramki ucięte przerwa
    uint32_t overflow;     // ramki zgubione przez pelny bufor
    uint32_t txDone;
    uint32_t txAborted;    // nadawanie przerwane (master przestal taktowac)
    uint32_t txLate;       // odpowiedzi odrzucone, bo slot juz minal
    uint32_t breaks;
};
// Zwraca liczniki od poprzedniego wywolania i je zeruje.
void takeStats(Stats& out);

} // namespace UnilinkBus

#endif // UNILINK_BUS_H
