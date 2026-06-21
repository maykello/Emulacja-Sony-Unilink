# ============================================================================
# Host-side (native) test build for the Sony UniLink CD-changer emulator.
#
# Purpose (spec: unilink-kompendium-alignment, task 1.1):
#   Build the PURE protocol-logic modules of the emulator on the development
#   host (no ESP32, no Arduino.h) so they can be exercised with property-based
#   tests using RapidCheck (>= 100 iterations per property).
#
# The emulator's pure modules (UnilinkFrame, CdText, Magazine, status mapping,
# etc.) depend only on <stdint.h>/<stddef.h> and therefore compile natively.
# Hardware modules (UnilinkBus ISR, Slave Break timing) are intentionally NOT
# part of this build.
#
# Usage (see run_tests.ps1 for a turn-key wrapper):
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   ctest --test-dir build --output-on-failure
# ============================================================================
cmake_minimum_required(VERSION 3.16)

project(unilink_emulator_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Directory that holds the emulator sources (one level up from test/).
set(EMULATOR_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ----------------------------------------------------------------------------
# RapidCheck dependency (property-based testing framework).
#
# Fetched as a source archive (no git required on the host). Pin to a branch
# archive for now; replace the URL with a specific commit archive
# (https://github.com/emil-e/rapidcheck/archive/<sha>.zip) to make the build
# fully reproducible.
# ----------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
  rapidcheck
  URL https://github.com/emil-e/rapidcheck/archive/refs/heads/master.zip
)
FetchContent_MakeAvailable(rapidcheck)

enable_testing()

# ----------------------------------------------------------------------------
# Pure, host-testable emulator modules.
#
# As pure logic modules are extracted (task 1.2 onward: UnilinkFrame.cpp, etc.)
# append them here so the tests link against the real production code. They MUST
# compile without Arduino.h. Keep this list empty until such a module exists.
# ----------------------------------------------------------------------------
set(EMULATOR_PURE_SOURCES
  "${EMULATOR_SRC_DIR}/UnilinkFrame.cpp"   # added in task 1.2
)

# ----------------------------------------------------------------------------
# Smoke test: verifies the native compile + RapidCheck run pipeline works.
# ----------------------------------------------------------------------------
add_executable(test_smoke
  test_smoke.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_smoke PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_smoke PRIVATE rapidcheck)

add_test(NAME test_smoke COMMAND test_smoke)

# Enforce >= 100 iterations per property for every test in this build.
# RapidCheck reads RC_PARAMS from the environment; max_success is the number of
# successful cases generated per property.
set_tests_properties(test_smoke PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 3 (task 1.3): dlugosc ramki wyznaczona przez CMD1 (UnilinkFrame).
# Validates: Requirements 3.1, 3.2, 3.3
# ----------------------------------------------------------------------------
add_executable(test_unilink_frame_length
  test_unilink_frame_length.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_unilink_frame_length PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_unilink_frame_length PRIVATE rapidcheck)

add_test(NAME test_unilink_frame_length COMMAND test_unilink_frame_length)

set_tests_properties(test_unilink_frame_length PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 1 (task 1.4): round-trip walidacji sum kontrolnych nadawanych ramek.
# Validates: Requirements 2.1, 2.4
# ----------------------------------------------------------------------------
add_executable(test_unilink_frame_roundtrip
  test_unilink_frame_roundtrip.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_unilink_frame_roundtrip PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_unilink_frame_roundtrip PRIVATE rapidcheck)

add_test(NAME test_unilink_frame_roundtrip COMMAND test_unilink_frame_roundtrip)

set_tests_properties(test_unilink_frame_roundtrip PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 22 (task 1.6): round-trip kodowania BCD czasu (UnilinkFrame).
# Validates: Requirements 11.1, 11.2
# ----------------------------------------------------------------------------
add_executable(test_unilink_frame_bcd
  test_unilink_frame_bcd.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_unilink_frame_bcd PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_unilink_frame_bcd PRIVATE rapidcheck)

add_test(NAME test_unilink_frame_bcd COMMAND test_unilink_frame_bcd)

set_tests_properties(test_unilink_frame_bcd PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 11 (task 1.7): round-trip kodowania numeru plyty F|nr (UnilinkFrame).
# Validates: Requirements 7.4
# ----------------------------------------------------------------------------
add_executable(test_unilink_frame_disc
  test_unilink_frame_disc.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_unilink_frame_disc PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_unilink_frame_disc PRIVATE rapidcheck)

add_test(NAME test_unilink_frame_disc COMMAND test_unilink_frame_disc)

set_tests_properties(test_unilink_frame_disc PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 2 (task 1.5): walidator odrzuca przeklamane ramki (UnilinkFrame).
# Validates: Requirements 2.1, 2.2, 2.3
# ----------------------------------------------------------------------------
add_executable(test_unilink_frame_corrupt
  test_unilink_frame_corrupt.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_unilink_frame_corrupt PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_unilink_frame_corrupt PRIVATE rapidcheck)

add_test(NAME test_unilink_frame_corrupt COMMAND test_unilink_frame_corrupt)

set_tests_properties(test_unilink_frame_corrupt PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Unit test (task 2.2): zgodnosc nadawanych ramek short/middle/long.
# Buduje konkretne przykladowe ramki odwzorowujace uklad/parzystosci funkcji
# nadawczych (UnilinkBus::sendShort/sendMedium/sendLong) i sprawdza, ze
# UnilinkFrame::validate zwraca Ok. To test JEDNOSTKOWY (konkretne przypadki),
# nie property — nie wymaga RapidCheck.
# Validates: Requirements 2.1, 2.4
# ----------------------------------------------------------------------------
add_executable(test_unilink_frame_send_compat
  test_unilink_frame_send_compat.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_unilink_frame_send_compat PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_unilink_frame_send_compat PRIVATE rapidcheck)

add_test(NAME test_unilink_frame_send_compat COMMAND test_unilink_frame_send_compat)

# ----------------------------------------------------------------------------
# Property 9 (task 9.5): wybor komendy CD-TEXT (CMD1) wg numeru pola (CdText).
# CdText.cpp nie jest (jeszcze) w EMULATOR_PURE_SOURCES, wiec dodajemy go
# jawnie do tego celu, obok wspolnych zrodel.
# Validates: Requirements 6.3, 6.4
# ----------------------------------------------------------------------------
add_executable(test_cdtext_command_field
  test_cdtext_command_field.cpp
  "${EMULATOR_SRC_DIR}/CdText.cpp"
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_cdtext_command_field PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_cdtext_command_field PRIVATE rapidcheck)

add_test(NAME test_cdtext_command_field COMMAND test_cdtext_command_field)

set_tests_properties(test_cdtext_command_field PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 8 (task 9.4): round-trip CD-TEXT (podzial na pola i zlozenie).
# Validates: Requirements 6.7, 6.8
# Uwaga: CdText.cpp NIE jest (jeszcze) w EMULATOR_PURE_SOURCES, wiec dodajemy go
# jawnie do listy zrodel tego targetu (unikamy wyscigu z 9.5/9.6).
# ----------------------------------------------------------------------------
add_executable(test_cdtext_roundtrip
  test_cdtext_roundtrip.cpp
  ${EMULATOR_PURE_SOURCES}
  "${EMULATOR_SRC_DIR}/CdText.cpp"
)
target_include_directories(test_cdtext_roundtrip PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_cdtext_roundtrip PRIVATE rapidcheck)

add_test(NAME test_cdtext_roundtrip COMMAND test_cdtext_roundtrip)

set_tests_properties(test_cdtext_roundtrip PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 10 (task 9.6): sanityzacja do drukowalnego ASCII (CdText).
# Validates: Requirements 6.6
# Uwaga: CdText.cpp NIE jest (jeszcze) w EMULATOR_PURE_SOURCES, wiec dodajemy go
# jawnie do listy zrodel tego targetu (unikamy wyscigu z 9.4/9.5).
# ----------------------------------------------------------------------------
add_executable(test_cdtext_sanitize_ascii
  test_cdtext_sanitize_ascii.cpp
  ${EMULATOR_PURE_SOURCES}
  "${EMULATOR_SRC_DIR}/CdText.cpp"
)
target_include_directories(test_cdtext_sanitize_ascii PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_cdtext_sanitize_ascii PRIVATE rapidcheck)

add_test(NAME test_cdtext_sanitize_ascii COMMAND test_cdtext_sanitize_ascii)

set_tests_properties(test_cdtext_sanitize_ascii PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 4 (task 3.2): parser tnie strumien po granicach wyznaczonych przez
# CMD1 (czysty model parsera UnilinkParserModel, bez ISR).
# Validates: Requirements 3.4, 3.5
# Uwaga: UnilinkParserModel.cpp NIE jest w EMULATOR_PURE_SOURCES, wiec dodajemy
# go jawnie do zrodel tego targetu (obok wspolnych zrodel).
# ----------------------------------------------------------------------------
add_executable(test_unilink_parser_boundaries
  test_unilink_parser_boundaries.cpp
  ${EMULATOR_PURE_SOURCES}
  "${EMULATOR_SRC_DIR}/UnilinkParserModel.cpp"
)
target_include_directories(test_unilink_parser_boundaries PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_unilink_parser_boundaries PRIVATE rapidcheck)

add_test(NAME test_unilink_parser_boundaries COMMAND test_unilink_parser_boundaries)

set_tests_properties(test_unilink_parser_boundaries PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 12 (task 10.3): mapa obecnosci plyt odzwierciedla dostepne plyty.
# Validates: Requirements 7.1
# Uwaga: Magazine::presenceMapFrom jest inline w Magazine.h (brak Magazine.cpp
# do linkowania dla tego helpera). Magazine.h dolacza UnilinkFrame.h, ktorego
# implementacja (UnilinkFrame.cpp) jest juz w EMULATOR_PURE_SOURCES.
# ----------------------------------------------------------------------------
add_executable(test_magazine_presence_map
  test_magazine_presence_map.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_magazine_presence_map PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_magazine_presence_map PRIVATE rapidcheck)

add_test(NAME test_magazine_presence_map COMMAND test_magazine_presence_map)

set_tests_properties(test_magazine_presence_map PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Property 13 (task 10.4): determinizm identyfikatora plyty (disc ID).
# Validates: Requirements 7.5
# Uwaga: Magazine::computeDiscId/buildDiscIdData sa inline w Magazine.h (brak
# Magazine.cpp do linkowania dla tych helperow). Magazine.h dolacza
# UnilinkFrame.h, ktorego implementacja (UnilinkFrame.cpp) jest juz w
# EMULATOR_PURE_SOURCES.
# ----------------------------------------------------------------------------
add_executable(test_magazine_disc_id_determinism
  test_magazine_disc_id_determinism.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_magazine_disc_id_determinism PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_magazine_disc_id_determinism PRIVATE rapidcheck)

add_test(NAME test_magazine_disc_id_determinism COMMAND test_magazine_disc_id_determinism)

set_tests_properties(test_magazine_disc_id_determinism PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)

# ----------------------------------------------------------------------------
# Unit test (task 5.5): odpowiedzi Anyone i startu (AddressManager).
# Validates: Requirements 4.1, 4.2
# Test jednostkowy (nie property-based), sprawdza konkretne sekwencje:
#   - start -> myAddr==0x30, allocated==false
#   -Anyone bez ID -> shouldSendDeviceInfo==true (device info 0x8C)
#   -Appoint z ID grupy CD -> allocated==true
#   -Anyone po przydzeniu -> shouldSendDeviceInfo==false
#   -Bus reset -> myAddr==0x30, allocated==false
# ----------------------------------------------------------------------------
add_executable(test_address_anyone
  test_address_anyone.cpp
)
target_include_directories(test_address_anyone PRIVATE "${EMULATOR_SRC_DIR}")

add_test(NAME test_address_anyone COMMAND test_address_anyone)
# ----------------------------------------------------------------------------
# Property 6 (task 5.4): niezmiennik adresu przed przydzialem ID (AddressManager).
# Validates: Requirements 4.1, 4.5
# Uwaga: Plik test_address_invariant.cpp nie istnieje (brak implementacji).
# Test jest pomijany do czasu implementacji.
# ----------------------------------------------------------------------------
# Property 5 (task 5.3): adopcja ID z grupy CD i powrot do 0x30 (AddressManager).
# Validates: Requirements 4.3, 4.4
# ----------------------------------------------------------------------------
add_executable(test_address_adopt_reset
  test_address_adopt_reset.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_address_adopt_reset PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_address_adopt_reset PRIVATE rapidcheck)

add_test(NAME test_address_adopt_reset COMMAND test_address_adopt_reset)

set_tests_properties(test_address_adopt_reset PROPERTIES
  ENVIRONMENT "RC_PARAMS=max_success=100"
)
# ----------------------------------------------------------------------------
# Task 4.2: Unit test Parity2 validation -> DROP (host-side, no Arduino.h).
# Validates: Requirements 2.5
# ----------------------------------------------------------------------------
add_executable(test_unilink_validate_drop
  test_unilink_validate_drop.cpp
  ${EMULATOR_PURE_SOURCES}
)
target_include_directories(test_unilink_validate_drop PRIVATE "${EMULATOR_SRC_DIR}")
target_link_libraries(test_unilink_validate_drop PRIVATE rapidcheck)

add_test(NAME test_unilink_validate_drop COMMAND test_unilink_validate_drop)
