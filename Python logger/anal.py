import re
import sys

def analyze_unilink_log(file_path):
    # Format 1: Całe ramki w jednej linii (np. aktywny sniffer)
    re_v1 = re.compile(r'(?:\[(.*?)\]\s*)?t=(\d+)\s+dt=(\d+)\s+bus=(\d)\s+:\s+(.*)')
    # Format 2: Bajt po bajcie ze znacznikami GAP
    re_v2 = re.compile(r'(?:\[(.*?)\]\s*)?\[GAP:\s*(\d+)\s*us\]\s*->\s*(.*)')
    
    # Słownik urządzeń (możesz dodawać własne)
    DEVICES = {
        0x10: "RADIO(Master)",
        0x18: "CD_CHANGER_1",
        0x3B: "MD_CHANGER/CD_2",
        0x30: "DEVICE_30",
        0x31: "DEVICE_31",
        0x70: "DISPLAY_70",
        0x71: "DISPLAY_71",
        0x91: "DEVICE_91",
        0xC1: "DEVICE_C1",
    }

    def get_dev_name(addr):
        return DEVICES.get(addr, f"0x{addr:02X}")

    def process_frame(frame_bytes, timestamp=""):
        if len(frame_bytes) < 5:
            return # Zbyt mało bajtów by utworzyć ramkę UniLink (odrzucamy śmieci / 0x00)
        
        dest = frame_bytes[0]
        src = frame_bytes[1]
        cmd = frame_bytes[2]
        subcmd = frame_bytes[3]
        chk1 = frame_bytes[4]
        
        # Weryfikacja pierwszej sumy kontrolnej
        calc_chk1 = (dest + src + cmd + subcmd) & 0xFF
        chk1_ok = (chk1 == calc_chk1)
        
        # Wyprowadzanie długości ładunku na podstawie komendy
        if cmd < 0x80:
            payload_len = 0
        elif cmd < 0xC0:
            payload_len = 4
        else:
            payload_len = 9
            
        payload = []
        chk2 = None
        chk2_ok = None
        
        # Weryfikacja drugiej sumy (jeśli występuje)
        if payload_len > 0 and len(frame_bytes) >= 5 + payload_len + 1:
            payload = frame_bytes[5 : 5+payload_len]
            chk2 = frame_bytes[5+payload_len]
            calc_chk2 = (chk1 + sum(payload)) & 0xFF
            chk2_ok = (chk2 == calc_chk2)
            
        direction = f"{get_dev_name(src)} -> {get_dev_name(dest)}"
        payload_hex = ' '.join(f'{b:02X}' for b in payload) if payload else "BRAK"
        
        chk1_str = f"OK({chk1:02X})" if chk1_ok else f"ERR({chk1:02X}!={calc_chk1:02X})"
        if payload_len > 0 and chk2 is not None:
            chk2_str = f"OK({chk2:02X})" if chk2_ok else f"ERR({chk2:02X}!={calc_chk2:02X})"
            chk_full = f"1: {chk1_str}, 2: {chk2_str}"
        else:
            chk_full = f"1: {chk1_str}"
            
        print(f"{timestamp:<12} | {direction:<30} | {cmd:02X}   | {subcmd:02X}  | {payload_hex:<26} | {chk_full}")

    # Nagłówek tabeli
    print("-" * 125)
    print(f"{'Czas logu':<12} | {'Kierunek':<30} | {'Cmd':<4} | {'Sub':<4} | {'Payload (Dane)':<26} | {'Sumy kontrolne'}")
    print("-" * 125)

    current_frame = []
    frame_timestamp = ""

    with open(file_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            
            # Łapanie czasu logowania (np. 14:47:21.520)
            ts_match = re.search(r'^\[(.*?)\]', line)
            ts = ts_match.group(1) if ts_match else ""

            # TEST 1: Sprawdź format 1 (cała ramka na raz)
            match_v1 = re_v1.search(line)
            if match_v1:
                # Jeśli w buforze były resztki, procesujemy je
                process_frame(current_frame, frame_timestamp)
                current_frame = []
                
                log_ts, t_us, dt_us, bus, hex_data = match_v1.groups()
                bytes_vals = [int(hp, 16) for hp in hex_data.strip().split() if hp != '!']
                process_frame(bytes_vals, log_ts if log_ts else t_us)
                continue
            
            # Wymuszenie przetworzenia bufora przez sniffer
            if "NOWA SEKWENCJA" in line:
                process_frame(current_frame, frame_timestamp)
                current_frame = []
                continue
            
            # TEST 2: Sprawdź format 2 (bajt po bajcie)
            match_v2 = re_v2.search(line)
            if match_v2:
                log_ts, gap_str, hex_str = match_v2.groups()
                gap = int(gap_str)
                
                try:
                    bytes_vals = [int(hp, 16) for hp in hex_str.strip().split() if hp != '!']
                except ValueError:
                    continue
                
                # Jeżeli przerwa w transmisji jest większa niż 4ms, to znak że leci NOWA ramka
                if gap > 4000:
                    process_frame(current_frame, frame_timestamp)
                    current_frame = []
                    frame_timestamp = log_ts if log_ts else ""
                    
                current_frame.extend(bytes_vals)

        # Na sam koniec procesu przepuść ostatnią, niedomkniętą ramkę
        process_frame(current_frame, frame_timestamp)

if __name__ == "__main__":
    if len(sys.argv) > 1:
        analyze_unilink_log(sys.argv[1])
    else:
        print("Użycie: python unilink_parser.py nazwa_logu.txt")