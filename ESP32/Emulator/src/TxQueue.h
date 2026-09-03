#ifndef TX_QUEUE_H
#define TX_QUEUE_H

#include <stdint.h>
#include <stddef.h>

// =============================================================================
// TxQueue — kolejka ramek do nadania po grancie 0x13 (Kompendium §7.2 / §12.4)
// =============================================================================
// Zmieniarka NIE nadaje od razu: kolejkuje blok odpowiedzi (status, disc ID,
// info magazynka, nazwy CD-TEXT, ramka czasu) i wysyla po jednej ramce na kazdy
// grant request-poll `0x01 0x13`. Ten modul przechowuje te oczekujace ramki.
//
// CECHY (Wymaganie 1.1, design.md "Cykl pracy i kolejka TX" + "Data Models"):
//   * STALY ROZMIAR (ring buffer), BEZ alokacji dynamicznej — caly bufor zyje
//     w obiekcie, bezpieczny na ESP32 (brak fragmentacji sterty w ISR-cyklu).
//   * dequeue zdejmuje ramke o NAJWYZSZYM priorytecie (najnizszy NUMER), a przy
//     rownym priorytecie zachowuje kolejnosc FIFO (najstarsza najpierw).
//   * przy PELNEJ kolejce enqueue porzuca NAJSTARSZA ramke o NAJNIZSZYM
//     priorytecie (najwyzszy numer), aby zrobic miejsce.
//
// Modul CELOWO zalezy tylko od <stdint.h>/<stddef.h> (bez Arduino.h, bez ISR,
// bez globalnego stanu sprzetu), dzieki czemu kompiluje sie natywnie na hoscie
// i daje sie testowac jednostkowo (zadanie 8.3) niezaleznie od ESP32.
// =============================================================================

namespace Tx {

// --- PRIORYTETY (nizszy numer = wyzszy priorytet), kolejnosc z design.md ---
//   status 0xC0 / PONG          -> 0 (najwyzszy)
//   disc ID 0xC5 / 0xD5         -> 1
//   info magazynka 0x95 / 0x97  -> 2
//   nazwy CD-TEXT               -> 3
//   ramka czasu 0x90            -> 4 (najnizszy)
constexpr uint8_t PRIO_STATUS    = 0;  // 0xC0 / PONG
constexpr uint8_t PRIO_DISC_ID   = 1;  // 0xC5 / 0xD5
constexpr uint8_t PRIO_MAGAZINE  = 2;  // 0x95 / 0x97
constexpr uint8_t PRIO_CD_TEXT   = 3;  // 0xC9 / 0xCD / 0xD9 / 0xDD / 0xD2
constexpr uint8_t PRIO_TIME      = 4;  // 0x90

// Maksymalna dlugosc ramki UniLink (long word = 16 bajtow z bajtem koncowym).
constexpr uint8_t TX_ITEM_MAX_BYTES = 16;

// Pojemnosc kolejki (stala, bez alokacji dynamicznej). Na cykl 0x13 wystarcza
// kilka ramek (status + disc ID + info + nazwy + czas); 16 daje zapas.
constexpr size_t TX_QUEUE_CAPACITY = 16;

// --- POJEDYNCZY ELEMENT KOLEJKI (design.md "Data Models") ---
struct TxItem {
    uint8_t priority;                    // 0 = najwyzszy priorytet
    uint8_t len;                         // liczba waznych bajtow w bytes[]
    uint8_t bytes[TX_ITEM_MAX_BYTES];    // gotowa do nadania ramka (z parzystosciami)
};

// =============================================================================
// TxQueue — ring o stalej pojemnosci, bez alokacji dynamicznej.
//
// Implementacja: tablica o stalym rozmiarze + monotoniczny licznik wstawien
// (seq) sluzacy do rozstrzygania kolejnosci FIFO w obrebie rownego priorytetu
// oraz do wskazania "najstarszej" ramki przy porzucaniu. Nie ma alokacji na
// stercie — caly stan zyje w obiekcie.
// =============================================================================
class TxQueue {
public:
    TxQueue();

    // Wstaw ramke do kolejki.
    // Gdy kolejka jest pelna, porzuca najstarsza ramke o najnizszym priorytecie
    // (najwyzszy numer), aby zrobic miejsce. Jezeli wstawiana ramka sama jest
    // scisle mniej wazna niz najmniej wazna ramka w kolejce, to ona zostaje
    // porzucona i element NIE jest zapamietany.
    // Zwraca true, jezeli element zostal zapamietany; false w przeciwnym razie.
    bool enqueue(const TxItem& item);

    // Wygodny wariant budujacy TxItem z surowych pol.
    // bytes==nullptr lub len>TX_ITEM_MAX_BYTES skutkuje obcieciem/zerowaniem.
    bool enqueue(uint8_t priority, const uint8_t* bytes, uint8_t len);

    // Zdejmij ramke o najwyzszym priorytecie (najnizszy numer); przy rownym
    // priorytecie zachowuje FIFO (najstarsza najpierw). Zwraca false, gdy pusto.
    bool dequeue(TxItem& out);

    bool   isEmpty() const { return count_ == 0; }
    bool   isFull()  const { return count_ == TX_QUEUE_CAPACITY; }

    // Ile ramek o DANYM priorytecie czeka w kolejce. Pozwala zapytac "czy blok
    // nazw juz zszedl" bez mylenia go z ramka statusu czy czasu — samo
    // isEmpty() do tego nie wystarcza i prowadzilo do gubienia zadan CD-TEXT.
    size_t countPriority(uint8_t priority) const {
        size_t n = 0;
        for (size_t i = 0; i < TX_QUEUE_CAPACITY; ++i) {
            if (slots_[i].used && slots_[i].item.priority == priority) n++;
        }
        return n;
    }

    size_t size()    const { return count_; }
    size_t capacity() const { return TX_QUEUE_CAPACITY; }

    // Usun wszystkie elementy (stan jak po konstrukcji, poza licznikiem seq).
    void clear();

private:
    // Wewnetrzny slot: element + znacznik kolejnosci wstawienia (FIFO/age).
    struct Slot {
        TxItem   item;
        uint32_t seq;     // mniejszy = starszy
        bool     used;
    };

    // Zwraca indeks slotu o najwyzszym priorytecie (najnizszy numer), przy
    // remisie najstarszego (min seq). -1 gdy kolejka pusta.
    int findHighestPriorityIndex() const;

    // Zwraca indeks slotu o najnizszym priorytecie (najwyzszy numer), przy
    // remisie najstarszego (min seq) — kandydat do porzucenia. -1 gdy pusto.
    int findDropCandidateIndex() const;

    Slot     slots_[TX_QUEUE_CAPACITY];
    size_t   count_;
    uint32_t nextSeq_;
};

} // namespace Tx

#endif // TX_QUEUE_H
