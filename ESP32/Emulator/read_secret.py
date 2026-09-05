import os

Import("env")

def get_wifi_password():
    user_profile = os.environ.get("USERPROFILE", "C:\\Users\\kontg")
    desktop_paths = [
        os.path.join(user_profile, "Desktop", "secret.txt"),
        os.path.join(user_profile, "Pulpit", "secret.txt"),
        os.path.join(os.getcwd(), "secret.txt")
    ]

    for path in desktop_paths:
        if os.path.exists(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    content = f.read().strip()
                    if content:
                        print(f"[read_secret.py] Wczytano haslo WiFi z: {path}")
                        return content
            except Exception as e:
                print(f"[read_secret.py] Blad odczytu {path}: {e}")

    print("[read_secret.py] OSTRZEZENIE: Nie znaleziono secret.txt na Pulpicie! Uzywam pustego hasla.")
    return ""

ssid = "Mayk3lGames2G"
password = get_wifi_password()

# Forwarding macro strings to compiler env
env.Append(CPPDEFINES=[
    ("WIFI_SSID", f'\\"{ssid}\\"'),
    ("WIFI_PASS", f'\\"{password}\\"')
])
