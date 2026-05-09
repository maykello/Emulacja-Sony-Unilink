import serial
import serial.tools.list_ports
import datetime
import time
import sys

def get_available_ports():
    ports = serial.tools.list_ports.comports()
    return ports

def main():
    print("=== SONY UNILINK LOGGER ===")
    
    ports = get_available_ports()
    if not ports:
        print("Nie znaleziono żadnych dostępnych urządzeń COM.")
        print("Sprawdź podłączenie Arduino lub ESP32.")
        sys.exit(1)
        
    print("\nDostępne urządzenia (Arduino / ESP32):")
    for i, port in enumerate(ports):
        print(f"[{i}] {port.device} - {port.description}")
        
    try:
        choice = int(input("\nWybierz numer urządzenia, z którego chcesz czytać logi: "))
        if choice < 0 or choice >= len(ports):
            print("Nieprawidłowy wybór.")
            sys.exit(1)
    except ValueError:
        print("Błędna wartość. Podaj liczbę przydzieloną do Twojego urządzenia.")
        sys.exit(1)
        
    selected_port = ports[choice].device
    baud_rate = 115200

    now = datetime.datetime.now()
    filename = now.strftime("unilink_log_%Y%m%d_%H%M%S.txt")
    
    print(f"\nUruchamiam nasłuch na porcie {selected_port} przy baudrate={baud_rate}...")
    print(f"Dane logów będą dodatkowo zapisywane do pliku: {filename}")
    print("Naciśnij Ctrl+C, aby zakończyć działanie programu.\n")
    print("-" * 50)
    
    try:
        ser = serial.Serial(selected_port, baud_rate, timeout=1)
        time.sleep(2)
        
        with open(filename, "a", encoding="utf-8") as f:
            while True:
                if ser.in_waiting > 0:
                    try:
                        line = ser.readline().decode('utf-8').strip()
                        if line:
                            timestamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                            log_entry = f"[{timestamp}] {line}"
                            
                            print(log_entry)
                            
                            f.write(log_entry + "\n")
                            f.flush()
                    except UnicodeDecodeError:
                        print("[Błąd dekodowania - ignoruję fragment]")
                
    except serial.SerialException as e:
        print(f"Błąd portu szeregowego: {e}")
    except KeyboardInterrupt:
        print("\n\nNasłuch zakończony przez użytkownika.")
    finally:
        if 'ser' in locals() and ser.is_open:
            ser.close()
        print(f"Logowanie zakończone pomyślnie. Plik znajduje się w: {filename}")

if __name__ == "__main__":
    main()
