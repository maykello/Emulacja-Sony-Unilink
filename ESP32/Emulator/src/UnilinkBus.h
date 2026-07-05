#ifndef UNILINK_BUS_H
#define UNILINK_BUS_H

#include <Arduino.h>

// =============================================================================
// UnilinkBus — warstwa fizyczna / lacza magistrali Sony UniLink
// =============================================================================
// Odpowiada WYLACZNIE za bit-banging: probkowanie bitow w przerwaniu zegara,
// skladanie bajtow do bufora RX, nadawanie ramek z bufora TX oraz Slave Break.
// Nie zna pojecia "zmieniarka", "utwor" ani protokolu aplikacyjnego — operuje
// tylko na surowych bajtach. Dzieki temu logike protokolu mozna testowac i
// rozwijac niezaleznie od sprzetu.
// =============================================================================

namespace UnilinkBus {

// Inicjalizacja pinow i podlaczenie przerwania zegara. Wywolac w setup().
void begin();

// --- NADAWANIE RAMEK ---
// Ramki UniLink: [RAD TAD CMD1 CMD2 PARITY1 (dane...) (PARITY2) END=0x00].
// Parzystosc liczona jest jako suma poprzedzajacych bajtow (& 0xFF).

// Short: 6 bajtow [RAD TAD CMD1 CMD2 PARITY 0x00].
void sendShort(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2);

// Medium: 11 bajtow z 4 bajtami danych i druga parzystoscia.
void sendMedium(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2,
                uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4);

// Long: 16 bajtow z 9 bajtami danych i druga parzystoscia.
void sendLong(uint8_t rad, uint8_t tad, uint8_t cmd1, uint8_t cmd2,
              uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4,
              uint8_t d5, uint8_t d6, uint8_t d7, uint8_t d8, uint8_t d9);

// Surowy pakiet z gotowymi checksumami (uzywany przy atrybutach/magic).
void sendRaw(const uint8_t* data, int len);

// Czy trwa nadawanie odpowiedzi? (nie przerywaj wlasnej transmisji)
bool isTransmitting();

// --- ODBIOR ---
// Mikrosekundy od ostatniego zbocza zegara (do wykrywania ciszy na magistrali).
// Odczyt atomowy (z wylaczonymi przerwaniami).
unsigned long microsSinceLastClock();

// Jesli magistrala jest cicho co najmniej `idleUs` i w buforze sa bajty,
// kopiuje je do `out` (max `maxLen`), czysci bufor RX i zwraca ich liczbe.
// W przeciwnym razie zwraca 0. Caly odczyt/reset wykonywany atomowo.
int readPacketIfIdle(uint8_t* out, int maxLen, unsigned long idleUs);

// Odczyt kompletnej ramki z granica wyznaczona przez CMD1 (Kompendium §3,
// Wymaganie 3.4/3.5). KRYTERIUM PODSTAWOWE: gdy w buforze sa >= 3 bajty,
// czytamy CMD1 (rxBuffer[2]) i wyznaczamy oczekiwana dlugosc przez
// UnilinkFrame::lengthFromCmd1; po zgromadzeniu tylu bajtow udostepniamy
// dokladnie tyle (reszta — poczatek kolejnej ramki — zostaje w buforze).
// Cisza (READ_SILENCE_US) sluzy WYLACZNIE jako zabezpieczenie awaryjne:
// gdy po ciszy w buforze tkwi niekompletny/nadmiarowy zlepek, bufor jest
// oprozniany (resynchronizacja), by uniknac zakleszczenia. Zwraca liczbe
// bajtow udostepnionej ramki lub 0. Caly odczyt/przesuw wykonywany atomowo.
int readFrame(uint8_t* out, int maxLen);

// Wyzeruj bufor odbiorczy (np. gdy BUS=0 — bajty z tej fazy sa "obce").
void resetRx();

// --- SLAVE BREAK ---
// Sciagniecie linii DATA w dol w fazie idle, by zasygnalizowac radiu chec
// aktualizacji wyswietlacza. Sam weryfikuje cisze tuz przed pociagnieciem i
// natychmiast porzuca break, gdy radio ruszy z zegarem (unika kolizji).
void issueSlaveBreak();

} // namespace UnilinkBus

#endif // UNILINK_BUS_H
