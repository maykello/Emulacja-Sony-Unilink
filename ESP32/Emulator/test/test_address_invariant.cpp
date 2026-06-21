// ============================================================================
// Property test: niezmiennik adresu przed przydzialem ID.
//
// Feature: unilink-kompendium-alignment, Property 6: Dla ciagu zdarzen bez Appoint TAD pozostaje 0x30 i nie przyjmuje stalego ID
// Validates: Requirements 4.1, 4.5
//
// Tlo: AddressManager to CZYSTA logika (struct State + pure apply), bez
// Arduino.h ani ISR, wiec reguly adresowania mozna przetestowac property-based
// natywnie na hoscie.
//
// Wymaganie 4.1: Emulator startuje z adresem 0x30 (grupa CD, brak ID).
// Wymaganie 4.5: Dopoki nie otrzyma przydzialu ID (Appoint), TAD pozostaje 0x30.
//
// Property: dla dowolnej sekwencji zdarzen, ktora nie zawiera Event::Appoint
// (tj. Start, Anyone, BusReset, None w dowolnej kombinacji), stan pozostaje
// niezmieniony: myAddr==0x30 oraz allocated==false.
// ============================================================================
#include <cstdint>
#include <vector>

#include <rapidcheck.h>

#include "AddressManager.h"

int main() {
    bool allPassed = true;

    // Property 6: dla dowolnej sekwencji zdarzen bez Appoint, TAD=0x30, allocated=false
    allPassed &= rc::check(
        "Property 6: niezmiennik adresu przed przydzialem ID",
        []() {
            // Wygeneruj losowa sekwencje zdarzen (1..5 elementow).
            // Dozwolone zdarzenia: Start, Anyone, BusReset, None (BEZ Appoint).
            const size_t seqLen =
                static_cast<size_t>(*rc::gen::inRange<size_t>(1, 6));

            std::vector<AddressManager::Event> events;
            for (size_t i = 0; i < seqLen; ++i) {
                // Wylosuj zdarzenie z zakresu 0..3 (Start=0, Anyone=1, BusReset=2, None=3)
                const int evIdx = *rc::gen::inRange(0, 4);
                switch (evIdx) {
                    case 0: events.push_back(AddressManager::Event::Start); break;
                    case 1: events.push_back(AddressManager::Event::Anyone); break;
                    case 2: events.push_back(AddressManager::Event::BusReset); break;
                    case 3: events.push_back(AddressManager::Event::None); break;
                }
            }

            // Symuluj przejscia stanu krok po kroku.
            AddressManager::State state = AddressManager::initial();

            // Sprawdz niezmiennik po kazdej zmianie.
            for (size_t i = 0; i < events.size(); ++i) {
                const AddressManager::Event ev = events[i];

                // Dla Event::Appoint nie powinnismy tutaj trafiac (sprawdzamy to w testcie).
                RC_ASSERT(ev != AddressManager::Event::Appoint);

                // Uzyj rad=0 (nieistotne, poniewaz Appoint nie wystepuje w sekwencji).
                state = AddressManager::apply(state, ev, 0);

                // Niezmiennik: TAD==0x30, allocated==false (R4.1, R4.5).
                RC_ASSERT(state.myAddr == 0x30);
                RC_ASSERT(state.allocated == false);
            }
        });

    return allPassed ? 0 : 1;
}
