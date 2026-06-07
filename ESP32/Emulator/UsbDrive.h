#ifndef USB_DRIVE_H
#define USB_DRIVE_H

#include <Arduino.h>
#include "FS.h"

// Inicjalizacja USB Host — uruchamia stos USB i czeka na pendrive'a.
// Wywołaj z setup() PRZED attachInterrupt (może używać delay()).
// Zwraca true jeśli stos USB uruchomiony poprawnie (nie znaczy że pendrive jest podpięty).
bool usbDriveInit();

// Czy pendrive jest zamontowany i gotowy do odczytu plików?
bool usbDriveIsMounted();

// Referencja do systemu plików USB (fs::FS) — do użycia z audio.connecttoFS().
// UWAGA: wywołuj TYLKO gdy usbDriveIsMounted() == true!
fs::FS& usbDriveGetFS();

#endif // USB_DRIVE_H
