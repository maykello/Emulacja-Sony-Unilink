// ============================================================================
// Property test: adopcja dowolnego ID z grupy CD i powrot do 0x30.
//
// Feature: unilink-kompendium-alignment, Property 5: Dla RAD z 0x31–0x3F po Appoint myAddr=RAD, a po Bus reset myAddr=0x30
// Validates: Requirements 4.3, 4.4
//
// Tlo: AddressManager to CZYSTA logika (struct State + pure apply), bez
// Arduino.h ani ISR, wiec reguly adresowania mozna przetestowac property-based
// natywnie na hoscie. Wymaganie 4.3: po Appoint (CMD1=0x02) z RAD z grupy CD
// menedzer przyjmuje przydzielone ID jako myAddr (TAD). Wymaganie 4.4: po Bus
// reset (0x01 0x00) menedzer przywraca adres grupowy 0x30.
// ============================================================================
#include <cstdint>

#include <rapidcheck.h>

#include "AddressManager.h"

int main() {
    bool allPassed = true;

    // Property 5: dla dowolnego RAD z grupy CD (0x31..0x3F):
    //  - apply(initial(), Appoint, rad) -> {myAddr==rad, allocated==true}  (R4.3)
    //  - apply(thatState, BusReset, *)  -> {myAddr==0x30, allocated==false} (R4.4)
    allPassed &= rc::check(
        "Property 5: adopcja ID z grupy CD i powrot do 0x30",
        []() {
            // RAD z zakresu 0x31..0x3F (grupa CD z przydzielonym ID).
            const uint8_t rad =
                static_cast<uint8_t>(*rc::gen::inRange(0x31, 0x40));

            // R4.3: Appoint adoptuje DOWOLNE ID z grupy CD jako myAddr.
            const AddressManager::State adopted = AddressManager::apply(
                AddressManager::initial(), AddressManager::Event::Appoint, rad);
            RC_ASSERT(adopted.myAddr == rad);
            RC_ASSERT(adopted.allocated == true);

            // R4.4: Bus reset przywraca adres grupowy 0x30 (allocated=false),
            // niezaleznie od wartosci RAD w ramce resetu.
            const uint8_t resetRad =
                static_cast<uint8_t>(*rc::gen::arbitrary<uint8_t>());
            const AddressManager::State afterReset = AddressManager::apply(
                adopted, AddressManager::Event::BusReset, resetRad);
            RC_ASSERT(afterReset.myAddr == 0x30);
            RC_ASSERT(afterReset.allocated == false);
        });

    return allPassed ? 0 : 1;
}
