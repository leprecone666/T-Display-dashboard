"""Feeds the T-Display with PC stats and Claude Code status over USB serial.

Run:  pythonw agent.py   (or `python agent.py` to see logs)
Needs: pip install pyserial psutil

The board reports the page it is showing ("PAGE,n"); the expensive collectors
(process scan, nvidia-smi, ping, media session) only run while their page is open.
Until the board reports a page (older firmware) everything runs.
"""
import argparse
import atexit
import json
import os
import subprocess
import tempfile
import threading
import time
import unicodedata
from datetime import datetime

import psutil
import serial
from serial.tools import list_ports

VID, PID = 0x1A86, 0x55D4  # CH9102
STATE_FILE = os.path.join(tempfile.gettempdir(), "claude_tdisplay.json")
USAGE_FILE = os.path.join(tempfile.gettempdir(), "claude_tdisplay_usage.json")
DONE_TO_IDLE_S = 30
HERE = os.path.dirname(os.path.abspath(__file__))
TARGETS_FILE = os.path.join(HERE, "targets.json")
MEDIA_SCRIPT = os.path.join(HERE, "media.ps1")
DEFAULT_TARGETS = [
    {"name": "Cloudflare", "host": "1.1.1.1"},
    {"name": "Google DNS", "host": "8.8.8.8"},
]
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)

# board pages, same numbering as in src/main.cpp
P_PC, P_CLAUDE, P_USAGE, P_TOP, P_GPU, P_NET, P_PING, P_CLOCK, P_MUSIC = range(9)

_TRANSLIT = dict(zip(
    "абвгдеёжзийклмнопрстуфхцчшщъыьэюяіїєґ",
    ["a", "b", "v", "g", "d", "e", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p", "r", "s", "t",
     "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya", "i", "yi", "ye", "g"]))


def ascii_text(s, limit=60):
    """The display fonts have no Cyrillic: transliterate, strip accents, drop the rest."""
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


class Need:
    """Which page the board shows. Collectors block here instead of polling:
    wait_for() sleeps until their page opens, sleep_while() until it closes."""

    def __init__(self):
        self._page = None  # unknown until the board reports one -> everything runs
        self.cond = threading.Condition()

    @property
    def page(self):
        return self._page

    @page.setter
    def page(self, v):
        with self.cond:
            if v != self._page:
                self._page = v
                self.cond.notify_all()

    def wants(self, *pages):
        p = self._page
        return p is None or p in pages

    def wait_for(self, *pages, timeout=None):
        with self.cond:
            self.cond.wait_for(lambda: self.wants(*pages), timeout)
        return self.wants(*pages)

    def sleep_while(self, seconds, *pages):
        """Sleep up to `seconds`, but wake early when the page is no longer one of `pages`."""
        with self.cond:
            self.cond.wait_for(lambda: not self.wants(*pages), seconds)


def find_port():
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            return p.device
    return None


class Reader(threading.Thread):
    """Reads what the board prints; only "PAGE,n" lines matter. Blocks in read()."""

    def __init__(self, need):
        super().__init__(daemon=True)
        self.need, self.ser, self.buf = need, None, b""
        self.ready = threading.Event()

    def connect(self, ser):
        self.ser, self.buf = ser, b""
        self.ready.set()

    def disconnect(self):
        self.ready.clear()
        self.ser = None

    def run(self):
        while True:
            self.ready.wait()
            ser = self.ser
            try:
                chunk = ser.read(256)  # returns after the 1 s port timeout at the latest
            except Exception:
                self.ready.clear()
                continue
            *lines, self.buf = (self.buf + chunk).split(b"\n")
            self.buf = self.buf[-64:]
            for ln in lines:
                ln = ln.strip()
                if ln.startswith(b"PAGE,") and ln[5:].isdigit():
                    self.need.page = int(ln[5:])


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

    def read(self, min_interval=2.0):
        """-> (util, temp); the full record is in self.vals."""
        if not self.ok:
            return -1.0, -1.0
        if time.time() - self.t >= min_interval:
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
    """Pings the hosts from targets.json (read-only ICMP echo) while the Ping page is open."""

    def __init__(self, need):
        super().__init__(daemon=True)
        self.need = need
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
            if not self.need.wants(P_PING):
                self.line = ""
                self.need.wait_for(P_PING)
            parts = []
            for t in self.load():
                name = str(t.get("name", t["host"])).replace(",", " ")[:11]
                parts.append(f"{name},{self.ping(t['host'])}")
            self.line = "PG," + ",".join(parts) + "\n"
            self.need.sleep_while(4, P_PING)


class Procs(threading.Thread):
    """Top processes by CPU (then RAM), aggregated by executable name, only on the Top page."""

    SKIP = {"System Idle Process", "System", "Registry", "Memory Compression"}

    def __init__(self, need):
        super().__init__(daemon=True)
        self.need = need
        self.line = ""
        self.ncpu = psutil.cpu_count() or 1

    def scan(self):
        agg = {}
        for p in psutil.process_iter(["name", "memory_info"]):
            try:
                name = p.info["name"] or "?"
                if name in self.SKIP:
                    continue
                cpu = p.cpu_percent(None) / self.ncpu
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
        return "TP," + ",".join(parts) + "\n"

    def run(self):
        primed = False
        while True:
            if not self.need.wants(P_TOP):
                self.line, primed = "", False
                self.need.wait_for(P_TOP)
            if not primed:  # cpu_percent needs a previous sample: prime, wait, then publish
                self.scan()
                time.sleep(1.0)
                primed = True
            self.line = self.scan()
            self.need.sleep_while(3, P_TOP)


class Media(threading.Thread):
    """Windows media session (Spotify, browsers, players). The PowerShell reader (~100 MB)
    runs only while the Music page is open and stops 8 s after leaving it."""

    EMPTY = "MD,none,,,0,0\n"

    def __init__(self, need):
        super().__init__(daemon=True)
        self.need = need
        self.line = ""
        self.proc = None
        atexit.register(self.stop)

    def stop(self):
        p, self.proc = self.proc, None
        if p and p.poll() is None:
            try:
                p.terminate()
            except Exception:
                pass

    def pump(self, p):
        try:
            for raw in p.stdout:
                f = raw.decode("utf-8", "replace").rstrip("\r\n").split("\t")
                if len(f) < 5:
                    self.line = self.EMPTY
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

    def ensure(self):
        if self.proc is None or self.proc.poll() is not None:
            try:
                self.line = self.EMPTY
                self.proc = subprocess.Popen(
                    ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", MEDIA_SCRIPT,
                     "-ParentPid", str(os.getpid())],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=NOWIN)
                threading.Thread(target=self.pump, args=(self.proc,), daemon=True).start()
            except Exception:
                time.sleep(3)

    def run(self):
        while True:
            self.need.wait_for(P_MUSIC)
            while self.need.wants(P_MUSIC):
                self.ensure()
                self.need.sleep_while(5, P_MUSIC)  # also notices a crashed reader
            if not self.need.wait_for(P_MUSIC, timeout=8):  # keep it 8 s in case the user comes back
                self.stop()
                self.line = ""


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
    s.timeout = 1.0  # the reader thread blocks in read(); writes are not affected
    s.open()
    return s


def go_low_priority():
    """Idle priority + Windows "efficiency mode" (EcoQoS): the agent never competes with real work."""
    try:
        psutil.Process().nice(psutil.IDLE_PRIORITY_CLASS)
    except Exception:
        pass
    try:
        import ctypes
        from ctypes import wintypes

        class PowerThrottling(ctypes.Structure):
            _fields_ = [("Version", wintypes.ULONG), ("ControlMask", wintypes.ULONG), ("StateMask", wintypes.ULONG)]

        state = PowerThrottling(1, 1, 1)  # version 1, control + enable PROCESS_POWER_THROTTLING_EXECUTION_SPEED
        k32 = ctypes.windll.kernel32
        k32.SetProcessInformation(k32.GetCurrentProcess(), 4, ctypes.byref(state), ctypes.sizeof(state))  # 4 = ProcessPowerThrottling
    except Exception:
        pass


def main():
    go_low_priority()
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="COM port (auto-detected by default)")
    args = ap.parse_args()

    need = Need()
    reader = Reader(need)
    gpu = Gpu()
    pinger, procs, media, weather = Pinger(need), Procs(need), Media(need), Weather()
    for t in (reader, pinger, procs, media, weather):
        t.start()
    psutil.cpu_percent(None)
    last_net = psutil.net_io_counters()
    last_t = time.time()
    boot = psutil.boot_time()
    disk, disk_at, tm_at = 0, 0.0, 0.0
    ser = None

    while True:
        try:
            if ser is None:
                port = args.port or find_port()
                if not port:
                    time.sleep(5)
                    continue
                ser = open_port(port)
                reader.connect(ser)
                tm_at = 0.0
                need.page = None  # unknown again until the board reports
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
            if now - disk_at > 15:  # disk usage barely moves
                disk = psutil.disk_usage(os.environ.get("SystemDrive", "C:") + "\\").percent
                disk_at = now
            uptime = int(now - boot)

            g_util, g_temp = -1.0, -1.0
            if need.wants(P_PC, P_GPU):
                g_util, g_temp = gpu.read(2.0 if need.wants(P_GPU) else 4.0)
            ser.write(f"PC,{cpu:.0f},{ram:.0f},{g_util:.0f},{disk:.0f},{down:.1f},{up:.1f},{uptime},{g_temp:.0f}\n".encode())
            ser.write(claude_line().encode())
            if now - tm_at > 30:  # the board keeps time on its own between updates
                ser.write(f"TM,{int(now) + time.localtime(now).tm_gmtoff}\n".encode())
                tm_at = now
            ser.write(f"LK,{int(is_locked())}\n".encode())
            if weather.line:
                ser.write(weather.line.encode())
            if need.wants(P_USAGE, P_CLAUDE):
                ul = usage_line()
                if ul:
                    ser.write(ul.encode())
            if need.wants(P_GPU) and gpu.line():
                ser.write(gpu.line().encode())
            if need.wants(P_TOP) and procs.line:
                ser.write(procs.line.encode())
            if need.wants(P_PING) and pinger.line:
                ser.write(pinger.line.encode())
            if need.wants(P_MUSIC) and media.line:
                ser.write(media.line.encode())
        except (serial.SerialException, OSError) as e:
            print("serial error:", e)
            reader.disconnect()
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
