"""Feeds the T-Display with PC stats and Claude Code status over USB serial.

Run:  pythonw agent.py   (or `python agent.py` to see logs)
Needs: pip install pyserial psutil
"""
import argparse
import json
import os
import subprocess
import tempfile
import threading
import time
from datetime import datetime, timezone

import psutil
import serial
from serial.tools import list_ports

VID, PID = 0x1A86, 0x55D4  # CH9102
STATE_FILE = os.path.join(tempfile.gettempdir(), "claude_tdisplay.json")
USAGE_FILE = os.path.join(tempfile.gettempdir(), "claude_tdisplay_usage.json")
DONE_TO_IDLE_S = 30
TARGETS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "targets.json")
DEFAULT_TARGETS = [
    {"name": "Cloudflare", "host": "1.1.1.1"},
    {"name": "Google DNS", "host": "8.8.8.8"},
]
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)
MEDIA_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "media.ps1")

_TRANSLIT = dict(zip(
    "абвгдеёжзийклмнопрстуфхцчшщъыьэюяіїєґ",
    ["a", "b", "v", "g", "d", "e", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p", "r", "s", "t",
     "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya", "i", "yi", "ye", "g"]))


def ascii_text(s, limit=60):
    """The display fonts have no Cyrillic: transliterate, strip accents, drop the rest."""
    import unicodedata
    out = []
    for ch in s:
        low = ch.lower()
        if low in _TRANSLIT:
            t = _TRANSLIT[low]
            out.append(t.capitalize() if ch != low else t)
        else:
            out.append(ch)
    s = unicodedata.normalize("NFKD", "".join(out))
    s = "".join(c if 32 <= ord(c) < 127 else "" for c in s)
    return s.replace(",", " ").replace("|", " ").strip()[:limit]


class Media(threading.Thread):
    """Reads the Windows media session (Spotify, browsers, players) via media.ps1."""

    def __init__(self):
        super().__init__(daemon=True)
        self.line = "MD,none,,,0,0\n"

    def run(self):
        while True:
            try:
                p = subprocess.Popen(
                    ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", MEDIA_SCRIPT],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=NOWIN)
                for raw in p.stdout:
                    f = raw.decode("utf-8", "replace").rstrip("\r\n").split("\t")
                    if len(f) < 5:
                        self.line = "MD,none,,,0,0\n"
                        continue
                    status = {"Playing": "playing", "Paused": "paused"}.get(f[0], "none")
                    title, artist = ascii_text(f[1]), ascii_text(f[2])
                    if not title:
                        status = "none"
                    pos = int(f[3]) if f[3].isdigit() else 0
                    dur = int(f[4]) if f[4].isdigit() else 0
                    self.line = f"MD,{status},{title},{artist},{pos},{dur}\n"
            except Exception:
                pass
            self.line = "MD,none,,,0,0\n"
            time.sleep(3)


def find_port():
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            return p.device
    return None


class Gpu:
    """NVIDIA stats via nvidia-smi; disables itself when unavailable."""

    FIELDS = "name,utilization.gpu,temperature.gpu,memory.used,memory.total,power.draw,power.limit,fan.speed,clocks.sm"

    def __init__(self):
        self.ok = True
        self.vals = None
        self.t = 0.0

    @staticmethod
    def _num(v):
        try:
            return float(v)
        except ValueError:
            return -1.0  # "[N/A]", "[Not Supported]"

    def read(self):
        """-> (util, temp); the full record is in self.vals."""
        if not self.ok:
            return -1.0, -1.0
        if time.time() - self.t >= 2:
            self.t = time.time()
            try:
                out = subprocess.run(
                    ["nvidia-smi", f"--query-gpu={self.FIELDS}", "--format=csv,noheader,nounits"],
                    capture_output=True, text=True, timeout=3, creationflags=NOWIN)
                f = [x.strip() for x in out.stdout.strip().splitlines()[0].split(",")]
                name = f[0].replace("NVIDIA ", "").replace("GeForce ", "")
                self.vals = [name] + [self._num(x) for x in f[1:9]]
            except Exception:
                self.ok = False
                self.vals = None
        if not self.vals:
            return -1.0, -1.0
        return self.vals[1], self.vals[2]

    def line(self):
        if not self.vals:
            return ""
        n, u, t, mu, mt, pw, pl, fan, clk = self.vals
        return f"GP,{ascii_text(n, 14)},{u:.0f},{t:.0f},{mu:.0f},{mt:.0f},{pw:.0f},{pl:.0f},{fan:.0f},{clk:.0f}\n"


class Pinger(threading.Thread):
    """Pings the hosts from targets.json every few seconds (read-only ICMP echo)."""

    def __init__(self):
        super().__init__(daemon=True)
        self.line = ""

    @staticmethod
    def load():
        try:
            with open(TARGETS_FILE, encoding="utf-8") as f:
                t = json.load(f)
            return [x for x in t if x.get("host")][:4] or DEFAULT_TARGETS
        except Exception:
            return DEFAULT_TARGETS

    @staticmethod
    def ping(host):
        try:
            out = subprocess.run(["ping", "-n", "1", "-w", "800", host],
                                 capture_output=True, text=True, timeout=3, creationflags=NOWIN).stdout
            for tok in out.replace("<", "=").split():
                if tok.lower().startswith(("time=", "время=")):
                    return int(float(tok.split("=")[1].rstrip("msмс")))
        except Exception:
            pass
        return -1

    def run(self):
        while True:
            parts = []
            for t in self.load():
                name = str(t.get("name", t["host"])).replace(",", " ")[:11]
                parts.append(f"{name},{self.ping(t['host'])}")
            self.line = "PG," + ",".join(parts) + "\n"
            time.sleep(3)


class Procs(threading.Thread):
    """Top processes by CPU (then RAM), aggregated by executable name."""

    SKIP = {"System Idle Process", "System", "Registry", "Memory Compression"}

    def __init__(self):
        super().__init__(daemon=True)
        self.line = ""

    def run(self):
        ncpu = psutil.cpu_count() or 1
        while True:
            agg = {}
            for p in psutil.process_iter(["name", "memory_info"]):
                try:
                    name = p.info["name"] or "?"
                    if name in self.SKIP:
                        continue
                    cpu = p.cpu_percent(None) / ncpu
                    mb = p.info["memory_info"].rss // 2**20
                    a = agg.setdefault(name, [0.0, 0])
                    a[0] += cpu
                    a[1] += mb
                except Exception:
                    pass
            top = sorted(agg.items(), key=lambda kv: (round(kv[1][0]), kv[1][1]), reverse=True)[:4]
            parts = []
            for name, (cpu, mb) in top:
                short = name[:-4] if name.lower().endswith(".exe") else name
                parts.append(f"{short.replace(',', ' ')[:10]},{cpu:.0f},{mb}")
            self.line = "TP," + ",".join(parts) + "\n"
            time.sleep(2)


def _minutes_until(v):
    """resets_at may be epoch seconds/ms or an ISO string; returns minutes (>=0) or -1."""
    try:
        if v is None:
            return -1
        if isinstance(v, (int, float)):
            t = v / 1000 if v > 1e11 else v
        else:
            t = datetime.fromisoformat(str(v).replace("Z", "+00:00")).timestamp()
        return max(int((t - time.time()) // 60), 0)
    except Exception:
        return -1


def usage_line():
    try:
        with open(USAGE_FILE, encoding="utf-8") as f:
            d = json.load(f)
    except Exception:
        return ""
    def pct(k):
        v = d.get(k)
        return -1 if v is None else int(round(float(v)))
    return f"US,{pct('five')},{_minutes_until(d.get('five_reset'))},{pct('week')},{_minutes_until(d.get('week_reset'))},{pct('ctx')}\n"


WEATHER_URL = ("https://api.open-meteo.com/v1/forecast?latitude=50.45&longitude=30.52"
               "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,relative_humidity_2m"
               "&daily=temperature_2m_max,temperature_2m_min&timezone=Europe%2FKyiv&forecast_days=1")


class Weather(threading.Thread):
    """Kyiv weather from open-meteo.com (free, no key), refreshed every 15 minutes."""

    def __init__(self):
        super().__init__(daemon=True)
        self.line = ""

    def run(self):
        import urllib.request
        while True:
            try:
                d = json.load(urllib.request.urlopen(WEATHER_URL, timeout=10))
                c, day = d["current"], d["daily"]
                self.line = (f"WX,{c['temperature_2m']:.0f},{c['apparent_temperature']:.0f},{c['weather_code']},"
                             f"{c['wind_speed_10m']:.0f},{c['relative_humidity_2m']:.0f},"
                             f"{day['temperature_2m_max'][0]:.0f},{day['temperature_2m_min'][0]:.0f}\n")
                time.sleep(900)
            except Exception:
                time.sleep(60)


def is_locked():
    """True while the Windows lock screen (secure desktop) is in front."""
    try:
        import ctypes
        h = ctypes.windll.user32.OpenInputDesktop(0, False, 0x0100)  # DESKTOP_SWITCHDESKTOP
        if not h:
            return True
        ctypes.windll.user32.CloseDesktop(h)
        return False
    except Exception:
        return False


def claude_line():
    state, tool, elapsed = "idle", "", 0
    try:
        with open(STATE_FILE, encoding="utf-8") as f:
            d = json.load(f)
        now = time.time()
        state = d.get("state", "idle")
        tool = str(d.get("tool", ""))
        if state in ("working", "waiting"):
            elapsed = int(now - d.get("work_start", now))
        elif state == "done":
            if now - d.get("state_since", now) > DONE_TO_IDLE_S:
                state = "idle"
            else:
                elapsed = int(d.get("dur", 0))
    except Exception:
        pass
    tool = tool.replace(",", " ").replace("\n", " ")[:24]
    return f"CL,{state},{elapsed},{tool}\n"


def open_port(name):
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.dtr = False  # DTR/RTS must stay low, otherwise the board resets
    s.rts = False
    s.timeout = 0
    s.open()
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="COM port (auto-detected by default)")
    args = ap.parse_args()

    gpu = Gpu()
    pinger = Pinger()
    pinger.start()
    procs = Procs()
    procs.start()
    media = Media()
    media.start()
    weather = Weather()
    weather.start()
    psutil.cpu_percent(None)
    last_net = psutil.net_io_counters()
    last_t = time.time()
    ser = None

    while True:
        try:
            if ser is None:
                port = args.port or find_port()
                if not port:
                    time.sleep(2)
                    continue
                ser = open_port(port)
                print("connected to", port)
                time.sleep(0.5)

            time.sleep(1.0)
            now = time.time()
            net = psutil.net_io_counters()
            dt = max(now - last_t, 0.1)
            down = (net.bytes_recv - last_net.bytes_recv) / 1024 / dt
            up = (net.bytes_sent - last_net.bytes_sent) / 1024 / dt
            last_net, last_t = net, now

            cpu = psutil.cpu_percent(None)
            ram = psutil.virtual_memory().percent
            disk = psutil.disk_usage(os.environ.get("SystemDrive", "C:") + "\\").percent
            uptime = int(now - psutil.boot_time())

            g_util, g_temp = gpu.read()
            ser.write(f"PC,{cpu:.0f},{ram:.0f},{g_util:.0f},{disk:.0f},{down:.1f},{up:.1f},{uptime},{g_temp:.0f}\n".encode())
            ser.write(claude_line().encode())
            ser.write(f"TM,{int(now) + time.localtime(now).tm_gmtoff}\n".encode())
            if pinger.line:
                ser.write(pinger.line.encode())
            if procs.line:
                ser.write(procs.line.encode())
            ser.write(media.line.encode())
            if gpu.line():
                ser.write(gpu.line().encode())
            if weather.line:
                ser.write(weather.line.encode())
            ser.write(f"LK,{int(is_locked())}\n".encode())
            ul = usage_line()
            if ul:
                ser.write(ul.encode())
            ser.read(1024)  # drain anything the board prints
        except (serial.SerialException, OSError) as e:
            print("serial error:", e)
            try:
                if ser:
                    ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(2)
        except KeyboardInterrupt:
            break


if __name__ == "__main__":
    main()
