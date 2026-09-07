import serial
import serial.tools.list_ports
import datetime
import time
import sys
import socket

def get_available_ports():
    return serial.tools.list_ports.comports()

def read_line_from_serial(ser):
    if ser.in_waiting > 0:
        try:
            return ser.readline().decode('utf-8', errors='replace').strip()
        except Exception:
            return None
    return None

def main():
    print("=== SONY UNILINK LOGGER ===")
    print("\nWybierz źródło logów:")
    print("[1] USB (Port COM)")
    print("[2] WiFi (unilink.local)")

    try:
        conn_choice = int(input("Wybierz opcję (1 lub 2): "))
        if conn_choice not in [1, 2]:
            print("Nieprawidłowy wybór.")
            sys.exit(1)
    except ValueError:
        print("Błędna wartość.")
        sys.exit(1)

    connection_type = "usb" if conn_choice == 1 else "wifi"
    selected_port = None
    wifi_host = "unilink.local"
    wifi_port = 12345

    if connection_type == "usb":
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
            print("Błędna wartość.")
            sys.exit(1)

        selected_port = ports[choice].device

    else: # wifi
        custom_host = input(f"\nPodaj adres ESP32 lub naciśnij Enter dla domyślnego [{wifi_host}]: ").strip()
        if custom_host:
            wifi_host = custom_host

    # Wybór urządzenia
    print("\nCo logujemy?")
    print("[1] Emulator")
    print("[2] Prawdziwa zmieniarka")

    try:
        device_choice = int(input("Wybierz opcję (1 lub 2): "))
        if device_choice not in [1, 2]:
            print("Nieprawidłowy wybór. Zamykam program.")
            sys.exit(1)
    except ValueError:
        print("Błędna wartość. Zamykam program.")
        sys.exit(1)

    device_name = "emulator" if device_choice == 1 else "zmieniarka"

    # Wybór modelu radia dla wybranego urządzenia
    print(f"\nDla jakiego radia pracuje {device_name}?")
    print("[1] MEX-BT3800U")
    print("[2] CDX-M670")

    try:
        radio_choice = int(input("Wybierz radio (1 lub 2): "))
        if radio_choice == 1:
            radio_name = "MEX-BT3800U"
        elif radio_choice == 2:
            radio_name = "CDX-M670"
        else:
            print("Nieprawidłowy wybór. Zamykam program.")
            sys.exit(1)
    except ValueError:
        print("Błędna wartość. Zamykam program.")
        sys.exit(1)

    log_prefix = f"unilink_log_{device_name}_{radio_name}_{connection_type}"
    baud_rate = 921600

    now = datetime.datetime.now()
    filename = now.strftime(f"{log_prefix}_%Y%m%d_%H%M%S.txt")

    if connection_type == "usb":
        print(f"\nUruchamiam nasłuch na porcie {selected_port} przy baudrate={baud_rate}...")
    else:
        print(f"\nŁączę z ESP32 po WiFi ({wifi_host}:{wifi_port})...")

    print(f"Dane logów będą zapisywane do pliku: {filename}")
    print("Naciśnij Ctrl+C, aby zakończyć działanie programu.\n")
    print("-" * 50)

    ser = None
    sock = None
    buffer = ""

    try:
        with open(filename, "a", encoding="utf-8") as f:
            while True:
                if connection_type == "usb":
                    if ser is None:
                        try:
                            print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] Próba połączenia z portem {selected_port}...")
                            ser = serial.Serial(selected_port, baud_rate, timeout=1)
                            time.sleep(1)
                            print(f"[USB] Pomyślnie połączono z {selected_port}\n")
                        except serial.SerialException as e:
                            print(f"[USB Błąd] Nie można otworzyć portu {selected_port}: {e}. Kolejna próba za 5 sekund...\n")
                            time.sleep(1)
                            continue

                    try:
                        line = read_line_from_serial(ser)
                        if line:
                            timestamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                            log_entry = f"[{timestamp}] {line}"
                            print(log_entry)
                            f.write(log_entry + "\n")
                            f.flush()
                        else:
                            time.sleep(0.01)
                    except (serial.SerialException, OSError) as e:
                        print(f"\n[USB] Utracono połączenie z portem (błąd: {e}). Próba wznowienia za 5 sekund...")
                        if ser:
                            ser.close()
                        ser = None
                        time.sleep(0.5)
                        continue

                else:
                    if sock is None:
                        try:
                            print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] Próba połączenia z {wifi_host}:{wifi_port}...")
                            sock = socket.create_connection((wifi_host, wifi_port), timeout=5)
                            sock.settimeout(1.0)
                            print(f"[WiFi] Pomyślnie połączono z {wifi_host}:{wifi_port}\n")
                        except Exception as e:
                            print(f"[WiFi Błąd] Nie można połączyć: {e}. Kolejna próba za 5 sekund...\n")
                            time.sleep(1)
                            continue

                    try:
                        chunk = sock.recv(4096).decode('utf-8', errors='replace')
                        if not chunk:
                            print("\n[WiFi] Połączenie zostało zamknięte przez ESP32. Próba wznowienia za 5 sekund...")
                            sock.close()
                            sock = None
                            time.sleep(1)
                            continue
                        buffer += chunk
                        while '\n' in buffer:
                            line, buffer = buffer.split('\n', 1)
                            line = line.strip()
                            if line:
                                timestamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                                log_entry = f"[{timestamp}] {line}"
                                print(log_entry)
                                f.write(log_entry + "\n")
                                f.flush()
                    except (socket.timeout, TimeoutError):
                        try:
                            # Wysyłamy ping (pusty znak), aby sprawdzić czy połączenie z ESP32 nadal żyje
                            # Jeśli ESP32 zostało zrestartowane (np. po włączeniu radia), to wyśle RST, co wyrzuci błąd.
                            sock.sendall(b'\n')
                        except Exception as e:
                            print(f"\n[WiFi] Wykryto zerwanie połączenia z ESP32 (ping failed: {e}). Próba wznowienia za 5 sekund...")
                            sock.close()
                            sock = None
                            time.sleep(1)
                        continue
                    except (ConnectionResetError, ConnectionAbortedError):
                        print("\n[WiFi] Połączenie zerwane przez serwer. Próba wznowienia za 5 sekund...")
                        sock.close()
                        sock = None
                        time.sleep(1)
                        continue
                    except Exception as e:
                        print(f"\n[WiFi Błąd połączenia] {e}. Próba wznowienia za 5 sekund...")
                        if sock:
                            sock.close()
                        sock = None
                        time.sleep(1)
                        continue


    except serial.SerialException as e:
        print(f"Błąd portu szeregowego: {e}")
    except KeyboardInterrupt:
        print("\n\nNasłuch zakończony przez użytkownika.")
    finally:
        if ser and ser.is_open:
            ser.close()
        if sock:
            sock.close()
        print(f"Logowanie zakończone pomyślnie. Plik znajduje się w: {filename}")

if __name__ == "__main__":
    main()