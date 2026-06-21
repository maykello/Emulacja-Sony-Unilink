// ============================================================================
// Unit test: odpowiedzi Anyone i startu (AddressManager).
//
// Feature: unilink-kompendium-alignment, Task 5.5
// Validates: Requirements 4.1, 4.2
//
// Tlo: Task 5.5 wymaga testu jednostkowego, ktory zweryfikuje:
//   - Adres startowy to 0x30 (grupa CD, brak ID) — R4.1, R4.5
//   - Na Anyone (0x01 0x02) bez przydzielonego ID wysyłany jest device info 0x8C — R4.2
//   - Po Appoint (przydzieleniu ID z grupy CD) emulator przestaje odpowiadać na Anyone
//   - Po Bus reset (0x01 0x00) adres wraca do 0x30 — R4.4
//
// Test jest JEDNOSTKOWY (nie property-based), bo testuje konkretne sekwencje
// zdarzeń i oczekiwane stany, a nie uniwersalne własciwosci.
// ============================================================================
#include <cassert>
#include <cstdint>
#include <iostream>

#include "AddressManager.h"

static bool testInitial() {
    // R4.1, R4.5: adres startowy to 0x30, allocated=false
    const AddressManager::State state = AddressManager::initial();
    if (state.myAddr != 0x30) {
        std::cerr << "FAIL: initial() myAddr=" << std::hex << (int)state.myAddr
                  << ", expected 0x30" << std::dec << std::endl;
        return false;
    }
    if (state.allocated) {
        std::cerr << "FAIL: initial() allocated=true, expected false" << std::endl;
        return false;
    }
    return true;
}

static bool testAnyoneBeforeAllocated() {
    // R4.2: na Anyone bez ID shouldSendDeviceInfo musi zwrocic true (device info 0x8C)
    const AddressManager::State state = AddressManager::initial();
    if (!AddressManager::shouldSendDeviceInfo(state, AddressManager::Event::Anyone)) {
        std::cerr << "FAIL: anyone before allocated: shouldSendDeviceInfo=false, expected true" << std::endl;
        return false;
    }
    return true;
}

static bool testAppointTakesId() {
    // R4.3: po Appoint z RAD z grupy CD myAddr=RAD, allocated=true
    const uint8_t appointedId = 0x35; // przykladowy ID z grupy CD
    const AddressManager::State adopted = AddressManager::apply(
        AddressManager::initial(), AddressManager::Event::Appoint, appointedId);
    if (adopted.myAddr != appointedId) {
        std::cerr << "FAIL: after Appoint myAddr=" << std::hex << (int)adopted.myAddr
                  << ", expected " << (int)appointedId << std::dec << std::endl;
        return false;
    }
    if (!adopted.allocated) {
        std::cerr << "FAIL: after Appoint allocated=false, expected true" << std::endl;
        return false;
    }
    return true;
}

static bool testAnyoneAfterAllocated() {
    // Po przydzieleniu ID na Anyone shouldSendDeviceInfo musi zwrocic false
    const uint8_t appointedId = 0x35;
    const AddressManager::State adopted = AddressManager::apply(
        AddressManager::initial(), AddressManager::Event::Appoint, appointedId);
    if (AddressManager::shouldSendDeviceInfo(adopted, AddressManager::Event::Anyone)) {
        std::cerr << "FAIL: anyone after allocated: shouldSendDeviceInfo=true, expected false" << std::endl;
        return false;
    }
    return true;
}

static bool testBusReset() {
    // R4.4: po Bus reset adres wraca do 0x30, allocated=false
    const uint8_t appointedId = 0x35;
    const AddressManager::State adopted = AddressManager::apply(
        AddressManager::initial(), AddressManager::Event::Appoint, appointedId);
    const AddressManager::State afterReset = AddressManager::apply(
        adopted, AddressManager::Event::BusReset, 0x00); // RAD w resetu nieistotny

    if (afterReset.myAddr != 0x30) {
        std::cerr << "FAIL: after BusReset myAddr=" << std::hex << (int)afterReset.myAddr
                  << ", expected 0x30" << std::dec << std::endl;
        return false;
    }
    if (afterReset.allocated) {
        std::cerr << "FAIL: after BusReset allocated=true, expected false" << std::endl;
        return false;
    }
    return true;
}

static bool testBusResetFromUnallocated() {
    // Bus reset z nieprzydzielonego stanu rowniez wraca do 0x30
    const AddressManager::State initial = AddressManager::initial();
    const AddressManager::State afterReset = AddressManager::apply(
        initial, AddressManager::Event::BusReset, 0x00);

    if (afterReset.myAddr != 0x30) {
        std::cerr << "FAIL: BusReset from unallocated myAddr=" << std::hex << (int)afterReset.myAddr
                  << ", expected 0x30" << std::dec << std::endl;
        return false;
    }
    if (afterReset.allocated) {
        std::cerr << "FAIL: BusReset from unallocated allocated=true, expected false" << std::endl;
        return false;
    }
    return true;
}

int main() {
    int failures = 0;

    std::cout << "Running AddressManager ANYONE/start unit tests..." << std::endl;

    if (!testInitial()) ++failures;
    if (!testAnyoneBeforeAllocated()) ++failures;
    if (!testAppointTakesId()) ++failures;
    if (!testAnyoneAfterAllocated()) ++failures;
    if (!testBusReset()) ++failures;
    if (!testBusResetFromUnallocated()) ++failures;

    if (failures == 0) {
        std::cout << "All tests PASSED." << std::endl;
        std::cout << "Startup address is 0x30 (grupa CD, brak ID)." << std::endl;
        return 0;
    } else {
        std::cerr << failures << " test(s) FAILED." << std::endl;
        return 1;
    }
}
