// =============================================================================
// UnilinkBus.cpp — warstwa fizyczna magistrali Sony Unilink
// =============================================================================
#include "UnilinkBus.h"
#include "Config.h"
#include "driver/gpio.h"
#include "driver/pcnt.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include "soc/pcnt_reg.h"

namespace UnilinkBus {

namespace {

constexpr uint32_t DATA_MASK   = 1UL << PIN_DATA;
constexpr uint32_t CLOCK_MASK  = 1UL << PIN_CLOCK;
constexpr uint32_t BUS_ON_MASK = 1UL << PIN_BUS_ON;
constexpr uint32_t RING_SIZE   = 32;   // potega dwojki
constexpr uint32_t RING_MASK   = RING_SIZE - 1;

// Wszystko, czego dotyka przerwanie, lezy w DRAM (zmienne statyczne). Przerwanie
// nie moze wolac kodu z flash ani czytac stalych z flash — dlatego kopiowanie
// idzie przez wskazniki volatile (kompilator nie zamieni petli na memcpy), a
// zamiast tablic lookup sa porownania.
portMUX_TYPE s_mux  = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t s_task = nullptr;
bool         s_isrIram = false;

volatile uint32_t s_lastEdgeUs = 0;
volatile uint32_t s_edgeCount  = 0;

// --- odbior ---
uint8_t s_rxBuf[FRAME_MAX];
uint8_t s_rxLen      = 0;
uint8_t s_rxBit      = 0;
uint8_t s_rxByte     = 0;
uint8_t s_rxExpected = 0;
bool    s_rxSkip     = false;   // ignoruj bity do najblizszej przerwy miedzy ramkami

Frame             s_ring[RING_SIZE];
volatile uint32_t s_ringHead = 0;   // pisze przerwanie
volatile uint32_t s_ringTail = 0;   // pisze zadanie

// --- nadawanie ---
enum class TxKind : uint8_t { Other, Claim, Grant };

uint8_t         s_txBuf[FRAME_MAX];
uint8_t         s_txLen     = 0;
uint8_t         s_txByte    = 0;
uint8_t         s_txBit     = 0;
volatile bool   s_txActive  = false;
bool            s_txStarted = false;
uint32_t        s_txArmedUs = 0;
volatile TxKind s_txKind    = TxKind::Other;
volatile bool   s_breakHolding = false;

// --- szybka sciezka ---
volatile bool     s_fpEnabled   = false;
volatile uint8_t  s_fpPingAddr  = 0;
volatile uint8_t  s_fpStatus    = 0x80;
volatile uint8_t  s_fpMyAddr    = 0;
uint8_t           s_fpClaim[11];
volatile uint8_t  s_fpClaimMask = 0;
uint8_t           s_fpGrant[FRAME_MAX];
volatile uint8_t  s_fpGrantLen  = 0;
volatile uint8_t  s_fpGrantSavedLen = 0;
volatile uint32_t s_grantsServed = 0;
volatile uint32_t s_claimsSent   = 0;
volatile uint32_t s_lastClaimUs  = 0;

// --- licznik sprzetowy zboczy (PCNT) ---
// Liczy zbocza zegara niezaleznie od CPU. "Paczka" = zbocza od przerwy miedzy
// ramkami do nastepnej przerwy (ramka albo slot odpowiedzi).
bool     s_pcntOk     = false;
int32_t  s_pcntPrev   = 0;    // stan licznika na pierwszym zboczu biezacej paczki
uint32_t s_burstIsr   = 0;    // zbocza przyjete przez przerwanie w biezacej paczce
uint32_t s_burstGlitch = 0;
uint32_t s_burstSpike  = 0;
// Paczka zamknieta na biezacym zboczu (ramka, ktora wlasnie sie skonczyla).
uint8_t  s_lbPcnt = 0, s_lbIsr = 0, s_lbGlitch = 0, s_lbSpike = 0;
bool     s_lbValid = false;

// --- statystyki ---
volatile uint32_t s_stFrames = 0, s_stGlitches = 0, s_stSpikes = 0, s_stBroken = 0, s_stOverflow = 0;
volatile uint32_t s_stTxDone = 0, s_stTxAborted = 0, s_stTxLate = 0, s_stBreaks = 0;
volatile uint32_t s_stRecovered = 0, s_stLostIsr = 0, s_stLostPin = 0;

} // namespace

#define UL_NOW_US() ((uint32_t)esp_timer_get_time())
// Poziom LOGICZNY linii DATA: 1 = dominujacy.
#define UL_DATA_DOMINANT() ((((REG_READ(GPIO_IN_REG) & DATA_MASK) != 0) ? 1 : 0) != (INVERT_DATA ? 1 : 0))
#define UL_LATCH_DOMINANT()                                          \
    do {                                                             \
        if (INVERT_DATA) REG_WRITE(GPIO_OUT_W1TC_REG, DATA_MASK);    \
        else             REG_WRITE(GPIO_OUT_W1TS_REG, DATA_MASK);    \
    } while (0)
#define UL_LATCH_RECESSIVE()                                         \
    do {                                                             \
        if (INVERT_DATA) REG_WRITE(GPIO_OUT_W1TS_REG, DATA_MASK);    \
        else             REG_WRITE(GPIO_OUT_W1TC_REG, DATA_MASK);    \
    } while (0)
#define UL_OUTPUT_ON()  REG_WRITE(GPIO_ENABLE_W1TS_REG, DATA_MASK)
#define UL_OUTPUT_OFF() REG_WRITE(GPIO_ENABLE_W1TC_REG, DATA_MASK)
#define UL_COPY(dst, src, n)                                                   \
    do {                                                                       \
        volatile uint8_t* _d = (volatile uint8_t*)(dst);                       \
        const volatile uint8_t* _s = (const volatile uint8_t*)(src);           \
        for (int _i = 0; _i < (int)(n); ++_i) _d[_i] = _s[_i];                 \
    } while (0)
#define UL_RX_RESET()                                                          \
    do { s_rxLen = 0; s_rxBit = 0; s_rxByte = 0; s_rxExpected = 0; } while (0)
#define UL_ARM_TX(n, now, kind)                                                \
    do {                                                                       \
        s_txLen = (uint8_t)(n); s_txByte = 0; s_txBit = 0;                     \
        s_txStarted = false; s_txArmedUs = (now); s_txKind = (kind);           \
        s_txActive = true;                                                     \
    } while (0)
#define UL_TX_RELEASE()                                                        \
    do {                                                                       \
        UL_OUTPUT_OFF(); UL_LATCH_RECESSIVE();                                 \
        s_txActive = false; s_txKind = TxKind::Other;                          \
    } while (0)
#define UL_TX_ABORT_RESTORE()                                                  \
    do {                                                                       \
        if (s_txKind == TxKind::Grant && s_fpGrantLen == 0 && s_fpGrantSavedLen != 0) { \
            s_fpGrantLen = s_fpGrantSavedLen;                                  \
        }                                                                      \
        UL_TX_RELEASE();                                                       \
    } while (0)

// Odpowiedzi, ktorych master oczekuje najczesciej i bez ktorych resetuje
// magistrale. Przygotowane przez zadanie protokolu, wysylane bez jego udzialu.
static inline uint8_t __attribute__((always_inline)) fastPathReply(uint32_t now)
{
    if (!s_fpEnabled || s_txActive || s_breakHolding) return 0;
    if (s_rxLen != 6 || s_rxBuf[5] != 0x00 || s_rxBuf[2] != 0x01) return 0;
    if ((uint8_t)(s_rxBuf[0] + s_rxBuf[1] + s_rxBuf[2] + s_rxBuf[3]) != s_rxBuf[4]) return 0;

    const uint8_t rad = s_rxBuf[0];
    const uint8_t tad = s_rxBuf[1];
    const uint8_t c2  = s_rxBuf[3];

    if (c2 == 0x12) {
        const uint8_t pa = s_fpPingAddr;
        if (pa == 0 || rad != pa || (tad != ADDR_MASTER && tad != ADDR_DISPLAY)) return 0;
        s_txBuf[0] = tad;
        s_txBuf[1] = rad;
        s_txBuf[2] = 0x00;
        s_txBuf[3] = s_fpStatus;
        s_txBuf[4] = (uint8_t)(s_txBuf[0] + s_txBuf[1] + s_txBuf[2] + s_txBuf[3]);
        s_txBuf[5] = 0x00;
        UL_ARM_TX(6, now, TxKind::Other);
        return 6;
    }
    if (c2 == 0x15) {
        if (rad != ADDR_BROADCAST || tad != ADDR_MASTER) return 0;
        if (s_fpClaimMask == 0 || s_fpGrantLen == 0) return 0;
        UL_COPY(s_txBuf, s_fpClaim, 11);
        UL_ARM_TX(11, now, TxKind::Claim);
        s_claimsSent++;
        s_lastClaimUs = now;
        return 11;
    }
    if (c2 == 0x13) {
        const uint8_t me = s_fpMyAddr;
        const uint8_t n  = s_fpGrantLen;
        if (me == 0 || rad != me || (tad != ADDR_MASTER && tad != ADDR_DISPLAY) || n == 0) return 0;
        UL_COPY(s_txBuf, s_fpGrant, n);
        s_fpGrantSavedLen = n;
        s_fpGrantLen = 0;
        UL_ARM_TX(n, now, TxKind::Grant);
        return n;
    }
    return 0;
}

static inline void __attribute__((always_inline)) pushFrame(uint32_t now, uint8_t replyLen, bool broken,
                                                            bool recovered = false)
{
    const uint32_t head = s_ringHead;
    const uint32_t next = (head + 1) & RING_MASK;
    if (next == s_ringTail) {
        s_stOverflow++;
        return;
    }
    Frame& f = s_ring[head];
    UL_COPY(f.data, s_rxBuf, s_rxLen);
    f.len       = s_rxLen;
    f.replyLen  = replyLen;
    f.broken    = broken;
    f.recovered = recovered;
    if (replyLen) UL_COPY(f.reply, s_txBuf, replyLen);
    f.endUs   = now;
    f.edgeSeq = s_edgeCount;
    const bool diag = (broken || recovered) && s_lbValid;
    f.pcntEdges   = diag ? s_lbPcnt : 0;
    f.isrEdges    = diag ? s_lbIsr : 0;
    f.glitchEdges = diag ? s_lbGlitch : 0;
    f.spikeEdges  = diag ? s_lbSpike : 0;
    __asm__ __volatile__("" ::: "memory");
    s_ringHead = next;
}

static inline void __attribute__((always_inline)) frameComplete(uint32_t now)
{
    s_stFrames++;
    pushFrame(now, fastPathReply(now), false);
}

static inline uint8_t __attribute__((always_inline)) sat8(uint32_t v) { return v > 255 ? 255 : (uint8_t)v; }

// Wolane na pierwszym zboczu po przerwie: podsumowuje paczke, ktora sie skonczyla.
// Licznik PCNT zawiera juz biezace zbocze, wiec roznica = zbocza konczonej paczki.
static inline void __attribute__((always_inline)) closeBurst()
{
    s_lbValid = false;
    if (s_pcntOk) {
        const int32_t cnt = (int16_t)(REG_READ(PCNT_U0_CNT_REG) & 0xFFFF);
        const int32_t d   = cnt - s_pcntPrev;
        if (d >= 0) {
            s_lbPcnt   = sat8((uint32_t)d);
            s_lbIsr    = sat8(s_burstIsr);
            s_lbGlitch = sat8(s_burstGlitch);
            s_lbSpike  = sat8(s_burstSpike);
            s_lbValid  = true;
        }
        if (cnt > 30000) {
            // Zerowanie, zanim licznik dojdzie do limitu. Kolejne zbocze najwczesniej
            // za takt (~125 us), wiec niczego nie gubimy.
            REG_SET_BIT(PCNT_CTRL_REG, PCNT_PULSE_CNT_RST_U0);
            REG_CLR_BIT(PCNT_CTRL_REG, PCNT_PULSE_CNT_RST_U0);
            s_pcntPrev = 0;
        } else {
            s_pcntPrev = cnt;
        }
    }
    s_burstIsr    = 1;
    s_burstGlitch = 0;
    s_burstSpike  = 0;
}

// Ramka z brakujacym bitem: czy PCNT widzial zbocze, ktorego przerwanie nie przyjelo?
static inline void __attribute__((always_inline)) classifyLostBit()
{
    if (!s_lbValid || (REG_READ(GPIO_IN_REG) & BUS_ON_MASK) == 0) return;
    if (s_lbPcnt > s_lbIsr) s_stLostIsr++;
    else                    s_stLostPin++;
}

static inline bool __attribute__((always_inline)) isOneBitDeletion(uint64_t r47, uint64_t t48)
{
    // r47 = t48 z usunietym jednym bitem <=> istnieje p: bity przed p zgodne z t48,
    // bity od p zgodne z t48 przesunietym o 1. a = niezgodnosci wariantu "bez
    // przesuniecia", b = "z przesunieciem"; warunek: najstarszy bit a lezy nizej
    // niz najmlodszy bit b.
    constexpr uint64_t M47 = (1ULL << 47) - 1;
    const uint64_t a   = r47 ^ (t48 >> 1);
    const uint64_t b   = r47 ^ (t48 & M47);
    const uint64_t low = b ? (b & (~b + 1)) : (1ULL << 47);
    return a < low;
}

static inline uint64_t __attribute__((always_inline)) frame6(uint8_t rad, uint8_t tad, uint8_t c2)
{
    const uint8_t sum = (uint8_t)(rad + tad + 0x01 + c2);
    return ((uint64_t)rad << 40) | ((uint64_t)tad << 32) | ((uint64_t)0x01 << 24) |
           ((uint64_t)c2 << 16) | ((uint64_t)sum << 8);
}

// Master taktuje juz slot odpowiedzi, a odebralismy tylko 47 bitow 6-bajtowej
// ramki. Jezeli to ping / grant / 01 15 do nas z jednym zgubionym bitem,
// odtwarzamy ja i odpowiadamy w tym slocie (bez odpowiedzi na ping radio
// resetuje magistrale). Zwraca -1, gdy nie udalo sie odtworzyc ramki, a w
// przeciwnym razie dlugosc uzbrojonej odpowiedzi (0 = szybka sciezka nie odpowiada).
static inline int __attribute__((always_inline)) recoverSlippedFrame(uint32_t now)
{
    if (!s_fpEnabled || s_rxLen != 5 || s_rxBit != 7) return -1;
    const uint64_t r = ((uint64_t)s_rxBuf[0] << 39) | ((uint64_t)s_rxBuf[1] << 31) |
                       ((uint64_t)s_rxBuf[2] << 23) | ((uint64_t)s_rxBuf[3] << 15) |
                       ((uint64_t)s_rxBuf[4] << 7)  | (uint64_t)(s_rxByte & 0x7F);
    const uint8_t pa = s_fpPingAddr;
    const uint8_t me = s_fpMyAddr;
    uint64_t cand[5];
    int n = 0;
    if (pa != 0) {
        cand[n++] = frame6(pa, ADDR_MASTER, 0x12);
        cand[n++] = frame6(pa, ADDR_DISPLAY, 0x12);
    }
    if (me != 0) {
        cand[n++] = frame6(me, ADDR_MASTER, 0x13);
        cand[n++] = frame6(me, ADDR_DISPLAY, 0x13);
    }
    cand[n++] = frame6(ADDR_BROADCAST, ADDR_MASTER, 0x15);

    int hit = -1;
    for (int i = 0; i < n; ++i) {
        if (isOneBitDeletion(r, cand[i])) {
            if (hit >= 0) return -1;   // niejednoznaczne — nie zgadujemy
            hit = i;
        }
    }
    if (hit < 0) return -1;
    // Stale przesuniecia: zmienne przesuniecie 64-bit to wywolanie funkcji libgcc.
    const uint64_t t = cand[hit];
    s_rxBuf[0] = (uint8_t)(t >> 40);
    s_rxBuf[1] = (uint8_t)(t >> 32);
    s_rxBuf[2] = (uint8_t)(t >> 24);
    s_rxBuf[3] = (uint8_t)(t >> 16);
    s_rxBuf[4] = (uint8_t)(t >> 8);
    s_rxBuf[5] = (uint8_t)t;
    s_rxLen = 6;
    return fastPathReply(now);
}

static void IRAM_ATTR onClockEdge(void*)
{
    const uint32_t now = UL_NOW_US();
    bool notify = false;

    portENTER_CRITICAL_ISR(&s_mux);
    if (BUS_CLOCK_LEVEL_CHECK) {
        const bool clockHigh = (REG_READ(GPIO_IN_REG) & CLOCK_MASK) != 0;
        if (clockHigh != (CLOCK_EDGE == RISING)) {
            s_stSpikes++;
            s_burstSpike++;
            portEXIT_CRITICAL_ISR(&s_mux);
            return;
        }
    }
    const uint32_t gap = now - s_lastEdgeUs;
    if (gap < BUS_CLOCK_GLITCH_US) {
        // Odbicie/zaklocenie na linii zegara — prawdziwe zbocza dziela ~125 us.
        s_stGlitches++;
        s_burstGlitch++;
        portEXIT_CRITICAL_ISR(&s_mux);
        return;
    }
    s_lastEdgeUs = now;
    s_edgeCount++;

    const bool frameGap = gap > BUS_FRAME_GAP_US;
    if (frameGap) closeBurst();
    else          s_burstIsr++;

    // Jezeli master zaczal taktowac w trakcie trzymania naszego impulsu Slave Break,
    // NATYCHMIAST zwalniamy linie DATA na pierwszym zboczu zegara, by nie zewrzec
    // pierwszych bajtow ramki mastera (co powodowalo SYSTEM RESET / pusty ekran 0x70).
    if (s_breakHolding) {
        UL_OUTPUT_OFF();
        UL_LATCH_RECESSIVE();
        s_breakHolding = false;
        UL_RX_RESET();
        s_rxSkip = false;
    }

    // Pierwsze zbocze slotu po ramce, w ktorej zabraklo jednego bitu. Odtworzona
    // odpowiedz idzie juz od tego zbocza, tak jak przy normalnie odebranej ramce.
    if (frameGap && !s_txActive && !s_rxSkip && s_rxLen == 5 && s_rxBit == 7) {
        const int replyLen = recoverSlippedFrame(now);
        if (replyLen >= 0) {
            classifyLostBit();
            s_stRecovered++;
            pushFrame(now, (uint8_t)replyLen, false, true);
            notify = true;
            UL_RX_RESET();
            s_rxSkip = true;
        }
    }

    bool handled = false;
    if (s_txActive) {
        if (s_txStarted && frameGap) {
            // Master przestal taktowac nasza odpowiedz i zaczyna nowa ramke.
            // Natychmiast zwalniamy linie — inaczej nadalibysmy reszte bitow
            // w srodek jego ramki.
            UL_TX_ABORT_RESTORE();
            s_stTxAborted++;
        } else {
            const uint8_t bit = (uint8_t)((s_txBuf[s_txByte] >> (7 - s_txBit)) & 1u);
            if (bit) UL_LATCH_DOMINANT();
            else     UL_LATCH_RECESSIVE();
            if (!s_txStarted) {
                // Wyjscie wlaczamy dopiero na pierwszym takcie slotu, z juz
                // ustawionym poziomem — w przerwie miedzy ramkami linia jest wolna.
                UL_OUTPUT_ON();
                s_txStarted = true;
            }
            if (++s_txBit >= 8) {
                s_txBit = 0;
                if (++s_txByte >= s_txLen) {
                    // Ostatni bajt ramki to zawsze 0x00, wiec zwolnienie linii
                    // po jego ostatnim bicie nie zmienia odczytu mastera.
                    const TxKind doneKind = s_txKind;
                    UL_TX_RELEASE();
                    s_stTxDone++;
                    if (doneKind == TxKind::Grant) {
                        s_fpGrantSavedLen = 0;
                        s_grantsServed++;
                        notify = true;
                    }
                    UL_RX_RESET();
                    s_rxSkip = true;
                }
            }
            handled = true;
        }
    }

    if (!handled) {
        if (frameGap) {
            if (s_rxLen != 0 || s_rxBit != 0) {
                s_stBroken++;
                if (s_rxLen >= 2) {
                    classifyLostBit();
                    pushFrame(now, 0, true);
                    notify = true;
                }
            }
            UL_RX_RESET();
            s_rxSkip = false;
        }
        if (!s_rxSkip) {
            s_rxByte = (uint8_t)((s_rxByte << 1) | (uint8_t)UL_DATA_DOMINANT());
            if (++s_rxBit >= 8) {
                const uint8_t v = s_rxByte;
                s_rxBit  = 0;
                s_rxByte = 0;
                if (s_rxLen == 0 && (v < 0x10 || v == 0xFF)) {
                    // Pusty slot odpowiedzi (0x00) albo smiec — to nie poczatek ramki.
                    s_rxSkip = true;
                } else {
                    s_rxBuf[s_rxLen++] = v;
                    if (s_rxLen == 3) {
                        const uint8_t c1 = s_rxBuf[2];
                        s_rxExpected = (c1 < 0x80) ? 6 : ((c1 < 0xC0) ? 11 : 16);
                    }
                    if (s_rxLen >= 3 && s_rxLen >= s_rxExpected) {
                        frameComplete(now);
                        notify = true;
                        s_rxLen  = 0;
                        s_rxSkip = true;
                    }
                }
            }
        }
    }
    portEXIT_CRITICAL_ISR(&s_mux);

    if (notify && s_task != nullptr) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &woken);
        if (woken == pdTRUE) portYIELD_FROM_ISR();
    }
}

bool begin(TaskHandle_t notifyTask)
{
    s_task = notifyTask;

    pinMode(PIN_BUS_ON, INPUT);
    pinMode(PIN_CLOCK, INPUT);
    // INPUT: funkcja GPIO, wejscie wlaczone, wyjscie wylaczone. Dalej sterujemy
    // juz tylko bitem "output enable", wiec odczyt linii dziala caly czas.
    pinMode(PIN_DATA, INPUT);
    UL_LATCH_RECESSIVE();

    // PCNT na tym samym pinie co przerwanie (przez macierz GPIO) — tylko diagnostyka.
    // Filtr 20 cykli APB (0.25 us) gasi dzwonienie, a zbocze jest juz policzone,
    // zanim przerwanie odczyta licznik.
    pcnt_config_t pc = {};
    pc.pulse_gpio_num = PIN_CLOCK;
    pc.ctrl_gpio_num  = PCNT_PIN_NOT_USED;
    pc.channel        = PCNT_CHANNEL_0;
    pc.unit           = PCNT_UNIT_0;
    pc.pos_mode       = (CLOCK_EDGE == FALLING) ? PCNT_COUNT_DIS : PCNT_COUNT_INC;
    pc.neg_mode       = (CLOCK_EDGE == FALLING) ? PCNT_COUNT_INC : PCNT_COUNT_DIS;
    pc.lctrl_mode     = PCNT_MODE_KEEP;
    pc.hctrl_mode     = PCNT_MODE_KEEP;
    pc.counter_h_lim  = 32767;
    pc.counter_l_lim  = -32768;
    s_pcntOk = pcnt_unit_config(&pc) == ESP_OK &&
               pcnt_set_filter_value(PCNT_UNIT_0, 20) == ESP_OK &&
               pcnt_filter_enable(PCNT_UNIT_0) == ESP_OK &&
               pcnt_counter_pause(PCNT_UNIT_0) == ESP_OK &&
               pcnt_counter_clear(PCNT_UNIT_0) == ESP_OK &&
               pcnt_counter_resume(PCNT_UNIT_0) == ESP_OK;
    // pcnt_unit_config wlacza podciaganie na pinie — linia ma zostac jak dotad.
    gpio_set_pull_mode((gpio_num_t)PIN_CLOCK, GPIO_FLOATING);
    s_pcntPrev = 0;

    s_lastEdgeUs = UL_NOW_US();

    gpio_set_intr_type((gpio_num_t)PIN_CLOCK,
                       (CLOCK_EDGE == FALLING) ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE);

    // Priorytet 3: przerwanie zegara wywlaszcza USB/I2S/tick. IRAM: dziala
    // w trakcie zapisu flash. Gdy linia 3 jest zajeta, schodzimy nizej.
    const int flagSets[] = {
        ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
        ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL2,
        ESP_INTR_FLAG_IRAM,
        0,
    };
    esp_err_t err = ESP_FAIL;
    for (int flags : flagSets) {
        err = gpio_install_isr_service(flags);
        if (err == ESP_OK) {
            s_isrIram = (flags & ESP_INTR_FLAG_IRAM) != 0;
            break;
        }
        if (err == ESP_ERR_INVALID_STATE) break;   // zainstalowany wczesniej
        gpio_uninstall_isr_service();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

    return gpio_isr_handler_add((gpio_num_t)PIN_CLOCK, onClockEdge, nullptr) == ESP_OK;
}

bool isrInIram() { return s_isrIram; }
bool pcntActive() { return s_pcntOk; }

bool popFrame(Frame& out)
{
    const uint32_t tail = s_ringTail;
    if (tail == s_ringHead) return false;
    __asm__ __volatile__("" ::: "memory");
    out = s_ring[tail];
    __asm__ __volatile__("" ::: "memory");
    s_ringTail = (tail + 1) & RING_MASK;
    return true;
}

bool respond(const Frame& f, const uint8_t* bytes, int len)
{
    if (bytes == nullptr || len <= 0 || len > FRAME_MAX) return false;
    bool armed = false;
    portENTER_CRITICAL(&s_mux);
    const uint32_t now = UL_NOW_US();
    if (!s_txActive && !s_breakHolding && s_edgeCount == f.edgeSeq &&
        (uint32_t)(now - f.endUs) < BUS_RESPONSE_DEADLINE_US) {
        for (int i = 0; i < len; ++i) s_txBuf[i] = bytes[i];
        UL_ARM_TX(len, now, TxKind::Other);
        armed = true;
    } else {
        s_stTxLate++;
    }
    portEXIT_CRITICAL(&s_mux);
    return armed;
}

bool isTransmitting() { return s_txActive; }

void abortTx()
{
    portENTER_CRITICAL(&s_mux);
    if (s_txActive) {
        UL_TX_ABORT_RESTORE();
        s_stTxAborted++;
        UL_RX_RESET();
        s_rxSkip = true;
    }
    portEXIT_CRITICAL(&s_mux);
}

void service()
{
    portENTER_CRITICAL(&s_mux);
    if (s_txActive) {
        const uint32_t now = UL_NOW_US();
        const bool noSlot = !s_txStarted && (uint32_t)(now - s_txArmedUs) > BUS_TX_NO_SLOT_TIMEOUT_US;
        const bool stall  =  s_txStarted && (uint32_t)(now - s_lastEdgeUs) > BUS_TX_STALL_TIMEOUT_US;
        if (noSlot || stall) {
            // Master nie taktuje juz naszej odpowiedzi. Linia NIGDY nie moze
            // zostac trzymana — inaczej radio nie wystartuje magistrali.
            UL_TX_ABORT_RESTORE();
            s_stTxAborted++;
            UL_RX_RESET();
            s_rxSkip = true;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

void setFastPath(bool enabled, uint8_t pingAddr, uint8_t status, uint8_t myAddr)
{
    portENTER_CRITICAL(&s_mux);
    s_fpEnabled  = enabled;
    s_fpPingAddr = pingAddr;
    s_fpStatus   = status;
    s_fpMyAddr   = myAddr;
    portEXIT_CRITICAL(&s_mux);
}

void loadGrant(const uint8_t* bytes, int len, uint8_t claimMask)
{
    if (bytes == nullptr || len <= 0 || len > FRAME_MAX || claimMask == 0) return;
    const uint8_t p1 = (uint8_t)(0x10 + 0x18 + 0x82 + claimMask);
    const uint8_t claim[11] = { 0x10, 0x18, 0x82, claimMask, p1, 0x00, 0x00, 0x00, 0x00, p1, 0x00 };
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < len; ++i) s_fpGrant[i] = bytes[i];
    for (int i = 0; i < 11; ++i) s_fpClaim[i] = claim[i];
    s_fpClaimMask     = claimMask;
    s_fpGrantLen      = (uint8_t)len;
    s_fpGrantSavedLen = (uint8_t)len;
    portEXIT_CRITICAL(&s_mux);
}

void clearGrant()
{
    portENTER_CRITICAL(&s_mux);
    s_fpGrantLen      = 0;
    s_fpGrantSavedLen = 0;
    portEXIT_CRITICAL(&s_mux);
}

bool     grantLoaded()  { return s_fpGrantLen != 0; }
uint32_t grantsServed() { return s_grantsServed; }
uint32_t claimsSent()   { return s_claimsSent; }
uint32_t lastClaimUs()  { return s_lastClaimUs; }

BreakResult trySlaveBreak()
{
    const uint32_t t0 = UL_NOW_US();
    const uint32_t e0 = s_edgeCount;
    if (s_txActive || (uint32_t)(t0 - s_lastEdgeUs) < BREAK_QUIET_BEFORE_US) {
        return BreakResult::Busy;
    }
    // 0 = czekam na faze dominujaca, 1 = w fazie dominujacej, 2 = w fazie recesywnej
    uint8_t  phase = 0;
    uint32_t mark  = t0;
    for (;;) {
        const uint32_t t = UL_NOW_US();
        if (s_edgeCount != e0 || s_txActive) return BreakResult::Busy;
        if ((REG_READ(GPIO_IN_REG) & BUS_ON_MASK) == 0) return BreakResult::Busy;
        if ((uint32_t)(t - t0) > BREAK_SEARCH_MAX_US) return BreakResult::NoIdleWave;

        const bool dom = UL_DATA_DOMINANT() != 0;
        if (phase == 0) {
            if (dom) { phase = 1; mark = t; }
        } else if (phase == 1) {
            if (!dom) {
                if ((uint32_t)(t - mark) >= BREAK_IDLE_LOW_MIN_US) { phase = 2; mark = t; }
                else phase = 0;
            } else if ((uint32_t)(t - mark) > BREAK_IDLE_LOW_MAX_US) {
                return BreakResult::NoIdleWave;
            }
        } else {
            if (dom) { phase = 1; mark = t; }
            else if ((uint32_t)(t - mark) >= BREAK_SETTLE_US) break;
        }
        delayMicroseconds(20);
    }

    portENTER_CRITICAL(&s_mux);
    const bool quiet = (s_edgeCount == e0) && !s_txActive && (UL_DATA_DOMINANT() == 0);
    if (quiet) {
        UL_LATCH_DOMINANT();
        UL_OUTPUT_ON();
        s_breakHolding = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!quiet) return BreakResult::Busy;

    // Jesli w trakcie impulsu master ruszy z zegarem, onClockEdge natychmiast
    // puszcza linie i zeruje s_breakHolding, a my nie niszczymy jego ramki.
    const uint32_t holdStart = UL_NOW_US();
    while ((uint32_t)(UL_NOW_US() - holdStart) < BREAK_HOLD_US) {
        if (!s_breakHolding || s_edgeCount != e0) break;
        delayMicroseconds(20);
    }

    portENTER_CRITICAL(&s_mux);
    const bool completed = s_breakHolding && (s_edgeCount == e0);
    if (s_breakHolding) {
        UL_OUTPUT_OFF();
        UL_LATCH_RECESSIVE();
        s_breakHolding = false;
        UL_RX_RESET();
        s_rxSkip = true;
    }
    if (completed) s_stBreaks++;
    portEXIT_CRITICAL(&s_mux);
    return completed ? BreakResult::Done : BreakResult::Busy;
}

uint32_t nowUs()           { return UL_NOW_US(); }
uint32_t usSinceLastEdge() { return UL_NOW_US() - s_lastEdgeUs; }
uint32_t edgeCount()       { return s_edgeCount; }
bool     busOnRaw()        { return (REG_READ(GPIO_IN_REG) & BUS_ON_MASK) != 0; }

void takeStats(Stats& out)
{
    portENTER_CRITICAL(&s_mux);
    out.frames    = s_stFrames;    s_stFrames    = 0;
    out.glitches  = s_stGlitches;  s_stGlitches  = 0;
    out.spikes    = s_stSpikes;    s_stSpikes    = 0;
    out.broken    = s_stBroken;    s_stBroken    = 0;
    out.overflow  = s_stOverflow;  s_stOverflow  = 0;
    out.txDone    = s_stTxDone;    s_stTxDone    = 0;
    out.txAborted = s_stTxAborted; s_stTxAborted = 0;
    out.txLate    = s_stTxLate;    s_stTxLate    = 0;
    out.breaks    = s_stBreaks;    s_stBreaks    = 0;
    out.recovered = s_stRecovered; s_stRecovered = 0;
    out.lostIsr   = s_stLostIsr;   s_stLostIsr   = 0;
    out.lostPin   = s_stLostPin;   s_stLostPin   = 0;
    portEXIT_CRITICAL(&s_mux);
}

} // namespace UnilinkBus
