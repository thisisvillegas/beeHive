import socket
import urllib.request
import json
import time
import sys

ESP_IP = "192.168.0.26"
ESP_PORT = 5005
SIMHUB_PORT = 8888

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

debug = '--debug' in sys.argv

print("SimHub -> ESP32 Bridge")
print(f"Sending to {ESP_IP}:{ESP_PORT}")
if debug:
    print("DEBUG mode: will dump first API response")


def timespan_to_ms(ts):
    """Convert SimHub TimeSpan string (HH:MM:SS.fffffff) to milliseconds."""
    if ts is None or ts == "" or ts == "00:00:00":
        return 0
    try:
        if isinstance(ts, (int, float)):
            return int(ts)
        parts = str(ts).split(":")
        if len(parts) == 3:
            h, m, s = parts
            seconds = int(h) * 3600 + int(m) * 60 + float(s)
        elif len(parts) == 2:
            m, s = parts
            seconds = int(m) * 60 + float(s)
        else:
            seconds = float(parts[0])
        return int(seconds * 1000)
    except:
        return 0


def get_field(data, *keys):
    """Try multiple field names, checking both root and NewData."""
    for key in keys:
        if key in data:
            return data[key]
        if "NewData" in data and key in data["NewData"]:
            return data["NewData"][key]
    return None


def safe_int(val, default=0):
    try:
        return int(float(val))
    except:
        return default


while True:
    try:
        url = f"http://localhost:{SIMHUB_PORT}/api/getgamedata"
        response = urllib.request.urlopen(url, timeout=1)
        data = json.loads(response.read().decode())

        if debug:
            print("\n=== API Response (first 3000 chars) ===")
            print(json.dumps(data, indent=2, default=str)[:3000])
            print("=== End ===\n")
            debug = False

        # Speed
        speed = safe_int(get_field(data, "SpeedKmh", "speedKmh", "Speed"))

        # Lap times (TimeSpan -> ms)
        best_lap_ms = timespan_to_ms(get_field(data, "BestLapTime"))
        all_best_ms = timespan_to_ms(get_field(data, "AllTimeBest"))
        last_lap_ms = timespan_to_ms(get_field(data, "LastLapTime"))

        # Lap count
        current_lap = safe_int(get_field(data, "CurrentLap"))
        total_laps = safe_int(get_field(data, "TotalLaps"))

        # Position
        position = safe_int(get_field(data, "Position", "PlayerLeaderboardPosition"))
        opponents = safe_int(get_field(data, "OpponentsCount"))

        # Tyre wear (percentage remaining)
        twfl = safe_int(get_field(data, "TyreWearFrontLeft"))
        twfr = safe_int(get_field(data, "TyreWearFrontRight"))
        twrl = safe_int(get_field(data, "TyreWearRearLeft"))
        twrr = safe_int(get_field(data, "TyreWearRearRight"))

        # Tyre temperature (Celsius, whole numbers)
        ttfl = safe_int(get_field(data, "TyreTemperatureFrontLeft"))
        ttfr = safe_int(get_field(data, "TyreTemperatureFrontRight"))
        ttrl = safe_int(get_field(data, "TyreTemperatureRearLeft"))
        ttrr = safe_int(get_field(data, "TyreTemperatureRearRight"))

        # Build UDP message — always send all fields, ESP filters by view
        parts = [
            f"speed={speed}",
            f"bestlap={best_lap_ms}",
            f"allbest={all_best_ms}",
            f"lastlap={last_lap_ms}",
            f"lap={current_lap}",
            f"totallaps={total_laps}",
            f"pos={position}",
            f"opponents={opponents}",
            f"twfl={twfl}",
            f"twfr={twfr}",
            f"twrl={twrl}",
            f"twrr={twrr}",
            f"ttfl={ttfl}",
            f"ttfr={ttfr}",
            f"ttrl={ttrl}",
            f"ttrr={ttrr}",
        ]

        msg = ",".join(parts)
        sock.sendto(msg.encode(), (ESP_IP, ESP_PORT))

        # Terminal status
        status = f"Spd:{speed} Pos:{position}/{opponents+1} Lap:{current_lap}/{total_laps}"
        if best_lap_ms > 0:
            m = best_lap_ms // 60000
            s = (best_lap_ms % 60000) / 1000
            status += f" Best:{m}:{s:06.3f}"

        print(f"\r{status}        ", end="", flush=True)

    except Exception as e:
        print(f"\rWaiting for SimHub...    ", end="", flush=True)
    time.sleep(0.05)
