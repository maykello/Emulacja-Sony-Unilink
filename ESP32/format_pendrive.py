"""
Skrypt do formatowania pendrive'a jako MBR + FAT32.
UWAGA: Wymaga uprawnień administratora!
UWAGA: USUNIE WSZYSTKIE DANE z wybranego dysku!

Użycie:
  python format_pendrive.py

Skrypt wylistuje dostępne dyski USB i pozwoli wybrać który sformatować.
"""

import subprocess
import sys
import ctypes
import os

def is_admin():
    """Sprawdź czy skrypt jest uruchomiony jako administrator."""
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except:
        return False

def list_disks():
    """Wylistuj dyski za pomocą diskpart."""
    script = "list disk\n"
    result = subprocess.run(
        ["diskpart"],
        input=script,
        capture_output=True,
        text=True,
        timeout=10
    )
    print(result.stdout)
    return result.stdout

def format_disk(disk_number):
    """Sformatuj dysk jako MBR + FAT32."""
    script = f"""select disk {disk_number}
clean
convert mbr
create partition primary
format fs=fat32 quick label=UNILINK
assign
exit
"""
    print(f"\n=== Formatowanie dysku {disk_number} jako MBR + FAT32 ===")
    print("Komenda diskpart:")
    print(script)
    
    result = subprocess.run(
        ["diskpart"],
        input=script,
        capture_output=True,
        text=True,
        timeout=60
    )
    print(result.stdout)
    if result.returncode != 0:
        print(f"BŁĄD: {result.stderr}")
        return False
    return True

def main():
    if not is_admin():
        print("BŁĄD: Ten skrypt wymaga uprawnień administratora!")
        print("Kliknij prawym na PowerShell/CMD -> 'Uruchom jako administrator'")
        print("Następnie uruchom: python format_pendrive.py")
        input("\nNaciśnij Enter aby zamknąć...")
        sys.exit(1)
    
    print("=" * 60)
    print("  FORMATOWANIE PENDRIVE'a - MBR + FAT32")
    print("  Dla emulatora Sony UniLink CD Changer")
    print("=" * 60)
    print("\nUWAGA: Ten skrypt USUNIE WSZYSTKIE DANE z wybranego dysku!\n")
    
    print("Dostępne dyski:\n")
    list_disks()
    
    print("\nWpisz numer dysku pendrive'a (np. 1, 2, 3...)")
    print("NIE WYBIERAJ dysku 0 — to twój dysk systemowy!")
    
    try:
        disk_num = int(input("\nNumer dysku: "))
    except ValueError:
        print("Nieprawidłowy numer!")
        input("\nNaciśnij Enter aby zamknąć...")
        sys.exit(1)
    
    if disk_num == 0:
        print("ODMOWA: Nie można formatować dysku 0 (systemowego)!")
        input("\nNaciśnij Enter aby zamknąć...")
        sys.exit(1)
    
    confirm = input(f"\nCzy na pewno chcesz sformatować DYSK {disk_num}? (wpisz TAK): ")
    if confirm.strip().upper() != "TAK":
        print("Anulowano.")
        input("\nNaciśnij Enter aby zamknąć...")
        sys.exit(0)
    
    if format_disk(disk_num):
        print("\n✓ Pendrive sformatowany pomyślnie jako MBR + FAT32!")
        print("  Etykieta: UNILINK")
        print("\nTeraz utwórz foldery CD01, CD02, ... i wrzuć do nich pliki MP3.")
    else:
        print("\n✗ Wystąpił błąd podczas formatowania!")
    
    input("\nNaciśnij Enter aby zamknąć...")

if __name__ == "__main__":
    main()
