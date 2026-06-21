#include "TxQueue.h"

// =============================================================================
// TxQueue — implementacja (czysta logika, bez Arduino.h / ISR / sterty).
// Patrz TxQueue.h po opis semantyki priorytetow i polityki porzucania.
// =============================================================================

namespace Tx {

TxQueue::TxQueue()
    : count_(0), nextSeq_(0) {
    for (size_t i = 0; i < TX_QUEUE_CAPACITY; ++i) {
        slots_[i].used = false;
        slots_[i].seq  = 0;
        slots_[i].item.priority = 0;
        slots_[i].item.len      = 0;
        for (uint8_t b = 0; b < TX_ITEM_MAX_BYTES; ++b) {
            slots_[i].item.bytes[b] = 0;
        }
    }
}

void TxQueue::clear() {
    for (size_t i = 0; i < TX_QUEUE_CAPACITY; ++i) {
        slots_[i].used = false;
    }
    count_ = 0;
}

int TxQueue::findHighestPriorityIndex() const {
    int best = -1;
    for (size_t i = 0; i < TX_QUEUE_CAPACITY; ++i) {
        if (!slots_[i].used) {
            continue;
        }
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        // Wyzszy priorytet = nizszy numer. Przy remisie wybierz starszy (min seq).
        const Slot& cand = slots_[i];
        const Slot& cur  = slots_[best];
        if (cand.item.priority < cur.item.priority ||
            (cand.item.priority == cur.item.priority && cand.seq < cur.seq)) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

int TxQueue::findDropCandidateIndex() const {
    int worst = -1;
    for (size_t i = 0; i < TX_QUEUE_CAPACITY; ++i) {
        if (!slots_[i].used) {
            continue;
        }
        if (worst < 0) {
            worst = static_cast<int>(i);
            continue;
        }
        // Najnizszy priorytet = najwyzszy numer. Przy remisie porzuc najstarszy.
        const Slot& cand = slots_[i];
        const Slot& cur  = slots_[worst];
        if (cand.item.priority > cur.item.priority ||
            (cand.item.priority == cur.item.priority && cand.seq < cur.seq)) {
            worst = static_cast<int>(i);
        }
    }
    return worst;
}

bool TxQueue::enqueue(const TxItem& item) {
    // Znormalizuj kopie wejscia (przytnij len do rozmiaru bufora).
    TxItem norm = item;
    if (norm.len > TX_ITEM_MAX_BYTES) {
        norm.len = TX_ITEM_MAX_BYTES;
    }

    if (count_ < TX_QUEUE_CAPACITY) {
        // Jest miejsce — znajdz wolny slot.
        for (size_t i = 0; i < TX_QUEUE_CAPACITY; ++i) {
            if (!slots_[i].used) {
                slots_[i].item = norm;
                slots_[i].seq  = nextSeq_++;
                slots_[i].used = true;
                ++count_;
                return true;
            }
        }
        // Nie powinno sie zdarzyc (count_ < capacity implikuje wolny slot).
        return false;
    }

    // Kolejka pelna — wyznacz najmniej wazna ramke (najnizszy priorytet,
    // przy remisie najstarsza) jako kandydata do porzucenia.
    int drop = findDropCandidateIndex();
    if (drop < 0) {
        return false;  // teoretycznie nieosiagalne (pelna => sa elementy)
    }

    // Jezeli nowa ramka jest SCISLE mniej wazna niz najmniej wazna w kolejce,
    // to ona sama jest najnizszym priorytetem -> porzucamy nowa, nic nie zmieniamy.
    if (norm.priority > slots_[drop].item.priority) {
        return false;
    }

    // W przeciwnym razie porzuc najstarsza ramke o najnizszym priorytecie i
    // wstaw nowa w jej miejsce (przy rownym priorytecie nowa jest "swiezsza").
    slots_[drop].item = norm;
    slots_[drop].seq  = nextSeq_++;
    slots_[drop].used = true;   // pozostaje uzyty; count_ bez zmian
    return true;
}

bool TxQueue::enqueue(uint8_t priority, const uint8_t* bytes, uint8_t len) {
    TxItem item;
    item.priority = priority;
    if (len > TX_ITEM_MAX_BYTES) {
        len = TX_ITEM_MAX_BYTES;
    }
    item.len = len;
    for (uint8_t i = 0; i < TX_ITEM_MAX_BYTES; ++i) {
        item.bytes[i] = (bytes != nullptr && i < len) ? bytes[i] : 0;
    }
    return enqueue(item);
}

bool TxQueue::dequeue(TxItem& out) {
    int idx = findHighestPriorityIndex();
    if (idx < 0) {
        return false;  // pusto
    }
    out = slots_[idx].item;
    slots_[idx].used = false;
    --count_;
    return true;
}

} // namespace Tx
