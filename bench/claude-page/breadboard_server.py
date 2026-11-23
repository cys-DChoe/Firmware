"""nucleo-h563zi-breadboard MCP 서버: 브레드보드 센서(HSC 차압, ABP2 정압) 값을 MCP 툴로 노출함.

보드(NUCLEO-H563ZI, D:\\STM\\bench 펌웨어)는 1초마다 ST-LINK 가상 COM 포트로 이런 줄을 찍음:
  [period 1000 ms, read 13201 us] #12  HSC 정상 0.05 Pa 31.6 C  |  ABP2 정상 21.1 Pa 28.27 C (st 0x40)
맨 앞 [...] 는 펌웨어 측정 주기(CONFIG_BENCH_SAMPLE_PERIOD_MS)와 한 번 읽는 데 걸린 시간임.

줄은 두 길로 들어옴 (같은 줄이 둘 다로 오면 하나만 쌓음):
  1) MQTT: 보드가 랜선으로 PC Mosquitto(BENCH_MQTT_HOST, 기본 127.0.0.1:1883)에 보내는 h563/bench/line.
     계정은 D:\\STM\\mqtt\\credentials.txt (BENCH_MQTT_CRED 로 바꿈). 보드 상태는 h563/bench/status.
  2) 시리얼: 보드 USB 가 PC 에 꽂혀 있으면 COM 포트.
툴이 처음 불리면 둘 다 받기 시작해서 기록 버퍼(기본 3600줄 = 1초 주기로 1시간)에 쌓음.
툴 호출이 BENCH_IDLE_RELEASE 초(기본 60) 동안 없으면 COM 포트는 놓아서, 그 사이엔 다른 프로그램이 쓸 수 있음.

COM 포트는 한 프로그램만 열 수 있어서 수집기(collector.py)와 나눠 씀:
  - 포트를 잡은 쪽이 시리얼에서 읽은 측정 줄을 브로커의 h563/bench/usb 로 다시 보냄. 못 잡은 쪽은 그걸 받음.
    (랜선이 빠져 있어도 수집기는 CSV 를 남기고, 이 서버는 값을 보여 줌)
  - h563/bench/usb 로 줄이 오고 있으면(= 다른 쪽이 포트를 잡음) 이 서버는 포트를 열려고 하지 않음.
  - BENCH_LOG_DIR\\serial.pause 파일이 있는 동안은 둘 다 포트를 안 잡음 (시리얼 터미널을 쓸 때).

  get_sensors()                      -> 가장 최근 한 줄 {"id", "seq", "period_ms", "read_us", "hsc", "abp2", "raw", ...}
  get_history(after_id=0, limit=300) -> after_id 다음부터 쌓인 줄들 {"latest_id", "samples": [...], ...}  (메모리, 최근 6분)
  get_range(start_ms, end_ms, step_s) -> 수집기 CSV(디스크, 수집기가 켜져 있던 동안 전부)를 step_s 초 평균으로

포트는 환경변수 BENCH_PORT (기본 COM3), 속도 BENCH_BAUD (기본 115200).

기록: 받은 측정 줄은 BENCH_LOG_DIR (기본 D:\\STM\\logs) 아래 날짜 폴더에 시간별 파일로 남음.
  D:\\STM\\logs\\2026-11-18\\11.csv    엑셀용 (UTF-8 BOM)
  D:\\STM\\logs\\2026-11-18\\11.jsonl  한 줄에 측정 하나씩 JSON (raw 원문 포함)
측정 줄이 아닌 보드 안내(부팅, [네트] 주소 등)는 이 폴더의 board_serial.log 에 남음.
"""

import collections
import csv
import json
import os
import re
import sys
import threading
import time

import serial
from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations

PORT = os.environ.get("BENCH_PORT", "COM3")
BAUD = int(os.environ.get("BENCH_BAUD", "115200"))
HIST_MAX = int(os.environ.get("BENCH_HISTORY", "3600"))
IDLE_RELEASE_S = float(os.environ.get("BENCH_IDLE_RELEASE", "60"))
MQTT_HOST = os.environ.get("BENCH_MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("BENCH_MQTT_PORT", "1883"))
MQTT_TOPIC = os.environ.get("BENCH_MQTT_TOPIC", "h563/bench")
MQTT_CRED = os.environ.get("BENCH_MQTT_CRED", r"D:\STM\mqtt\credentials.txt")
FRESH_MS = 5000  # 이보다 최근에 줄이 들어왔으면 오류 안내를 안 띄움
USB_FRESH_MS = 5000  # 이보다 최근에 <토픽>/usb 로 줄이 왔으면 다른 프로그램(수집기)이 COM 포트를 잡고 있는 것

mcp = FastMCP("nucleo-h563zi-breadboard")

# 벤투리 유량 계산은 수집기와 같은 venturi.py 를 씀 (인계 폴더 server/ 에 있음)
_HERE = os.path.dirname(os.path.abspath(__file__))
for _d in (os.environ.get("BENCH_VENTURI_DIR"), os.path.join(_HERE, "..", "server"), r"D:\STM\migration\server"):
    if _d and os.path.exists(os.path.join(_d, "venturi.py")):
        sys.path.insert(0, _d)
        break
try:
    import venturi
    VENTURI = venturi.load_config()
except ImportError:  # 없으면 유량 없이 돎
    venturi = VENTURI = None

def _venturi_cfg():
    """벤투리 설정. 파일(venturi.json · venturi_cal.json)이 바뀌었으면 다시 읽음 (1초에 한 번만 확인)."""
    global VENTURI
    VENTURI = venturi.refresh(VENTURI)
    return VENTURI


ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# 측정 줄: "  |  " 로 나뉜 칸마다 센서 하나 (HSC · ABP2 는 꼭 있음, SDP810 · BME280 · DS18B20 은 펌웨어에 따라).
# 모르는 이름의 칸은 건너뜀. collector.py 와 같은 규칙.
LINE = re.compile(r"#(\d+)\s+(HSC\s+.+?\s+\|\s+ABP2\s+.+?)\s*$")
SEG = re.compile(r"\s+\|\s+")
TIMING = re.compile(r"\[period\s+(\d+)\s+ms,\s+read\s+(\d+)\s+us\]")
VALS = re.compile(r"^(\S+(?:\s\S+)*?)(?:\s+(-?[\d.]+)\s+Pa\s+(-?[\d.]+)\s+C)?(?:\s+\(st\s+(0x[0-9a-fA-F]+)\))?$")
# 환경 센서 칸: "정상 102039.6 Pa 23.92 C 21.5 %" (BME280) · "정상 24.31 C" (DS18B20) · "응답 없음"
ENV = re.compile(r"^(\S+(?:\s\S+)*?)(?:\s+(-?[\d.]+)\s+Pa)?(?:\s+(-?[\d.]+)\s+C)?(?:\s+(-?[\d.]+)\s+%)?$")


BOARD_LOG = os.environ.get("BENCH_BOARD_LOG", os.path.join(os.path.dirname(os.path.abspath(__file__)), "board_serial.log"))


def _board_log(line: str):
    """측정 줄이 아닌 보드 안내(부팅, [네트] 주소·연결 등)를 파일에 남김. 1 MB 넘으면 새로 씀."""
    try:
        mode = "w" if os.path.exists(BOARD_LOG) and os.path.getsize(BOARD_LOG) > 1_000_000 else "a"
        with open(BOARD_LOG, mode, encoding="utf-8") as f:
            f.write(f"{time.strftime('%m-%d %H:%M:%S')} {line}\n")
    except OSError:
        pass


def _sensor(text: str) -> dict:
    m = VALS.match(text.strip())
    if not m:
        return {"ok": False, "state": text.strip(), "pa": None, "c": None}
    state, pa, c, st = m.groups()
    out = {
        "ok": state == "정상",
        "state": state,
        "pa": float(pa) if pa is not None else None,
        "c": float(c) if c is not None else None,
    }
    if st is not None:
        out["status"] = st
    return out


def _env(text: str) -> dict:
    """BME280(대기압 Pa · 주변 온도 · 습도 %) · DS18B20(가스 온도) 칸. 없는 값은 None."""
    m = ENV.match(text.strip())
    if not m:
        return {"ok": False, "state": text.strip(), "pa": None, "c": None, "rh": None}
    state, pa, c, rh = m.groups()
    f = lambda v: float(v) if v is not None else None  # noqa: E731
    return {"ok": state == "정상", "state": state, "pa": f(pa), "c": f(c), "rh": f(rh)}


LOG_DIR = os.environ.get("BENCH_LOG_DIR", r"D:\STM\logs")
SERIAL_PAUSE = os.path.join(LOG_DIR, "serial.pause")  # 이 파일이 있는 동안은 COM 포트를 안 잡음 (수집기도 같음)
CSV_HEAD = ["time", "seq", "src", "period_ms", "read_us",
            "hsc_state", "hsc_pa", "hsc_c", "abp2_state", "abp2_pa", "abp2_c", "abp2_status",
            "sdp_state", "sdp_pa", "sdp_c"]


class SampleLog:
    """측정 줄을 LOG_DIR\\YYYY-MM-DD\\HH.csv 와 HH.jsonl 에 한 줄씩 덧붙임 (시간마다 새 파일).

    앱이 이 서버를 여러 개 띄우므로, LOG_DIR\\.writer.lock 을 잡은 하나만 씀.
    잡은 서버가 꺼지면 다른 서버가 30초 안에 이어받음."""

    def __init__(self):
        self.lockf = None
        self.owner = False
        self.next_try = 0.0
        self.err_shown = False
        self.checked = set()  # 머리줄을 확인한 CSV 경로

    def _own(self) -> bool:
        if self.owner:
            return True
        if time.time() < self.next_try:
            return False
        self.next_try = time.time() + 30
        try:
            os.makedirs(LOG_DIR, exist_ok=True)
            f = open(os.path.join(LOG_DIR, ".writer.lock"), "a+")
            try:
                if os.name == "nt":
                    import msvcrt

                    msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                else:  # 맥 · 리눅스
                    import fcntl

                    fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                f.close()
                return False
            self.lockf, self.owner = f, True
        except Exception:
            return False
        return True

    def write(self, r: dict):
        # 기록은 수집기(collector.py)가 맡음. 이 서버가 직접 남기게 하려면 BENCH_SERVER_LOG=1
        if os.environ.get("BENCH_SERVER_LOG", "0") != "1" or not self._own():
            return
        try:
            ts = time.localtime(r["t"] / 1000)
            day = os.path.join(LOG_DIR, time.strftime("%Y-%m-%d", ts))
            os.makedirs(day, exist_ok=True)
            base = os.path.join(day, time.strftime("%H", ts))
            iso = time.strftime("%Y-%m-%dT%H:%M:%S", ts) + f".{r['t'] % 1000:03d}"
            h, a, s = r["hsc"], r["abp2"], r.get("sdp") or {}
            vals = [iso, r["seq"], r["src"], r["period_ms"], r["read_us"],
                    h["state"], h["pa"], h["c"], a["state"], a["pa"], a["c"], a.get("status"),
                    s.get("state"), s.get("pa"), s.get("c")]
            csv_path = base + ".csv"
            new = not os.path.exists(csv_path)
            if not new and csv_path not in self.checked:  # 열이 바뀌기 전에 만든 파일이면 옆으로 치우고 새로 시작
                with open(csv_path, encoding="utf-8-sig") as f:
                    old_head = f.readline().strip() != ",".join(CSV_HEAD)
                if old_head:
                    os.replace(csv_path, base + f"_old{int(time.time())}.csv")
                    new = True
            self.checked.add(csv_path)
            with open(csv_path, "a", encoding="utf-8-sig" if new else "utf-8", newline="") as f:
                w = csv.writer(f)
                if new:
                    w.writerow(CSV_HEAD)
                w.writerow(["" if v is None else v for v in vals])
            with open(base + ".jsonl", "a", encoding="utf-8") as f:
                f.write(json.dumps({"time": iso, **{k: v for k, v in r.items() if k != "id"}},
                                   ensure_ascii=False) + "\n")
        except OSError:
            pass  # 디스크 문제로 기록이 막혀도 실시간 표시는 계속


sample_log = SampleLog()


class Reader:
    """COM 포트를 뒤에서 계속 읽어 측정 줄을 쌓아 둠. 호출이 끊기면 포트를 놓음."""

    def __init__(self):
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.buf = collections.deque(maxlen=HIST_MAX)
        self.next_id = 1
        self.last_call = 0.0
        self.thread = None
        self.port_open = False
        self.error = None
        self.note = None  # 측정 줄이 아닌 마지막 안내 줄 (예: "HSC 영점 7.13 Pa ...")
        self.period_ms = None
        self.mqtt = None
        self.mqtt_ok = False
        self.mqtt_error = None
        self.board = None  # 보드가 보낸 상태: "online 172.30.1.x" / "offline"
        self.last_mqtt_ms = 0  # 랜선(MQTT)으로 측정 줄이 마지막으로 온 시각
        self.last_usb_ms = 0   # 누군가 시리얼에서 읽어 <토픽>/usb 로 다시 보낸 줄이 마지막으로 온 시각

    def touch(self):
        with self.lock:
            self.last_call = time.time()
            if self.thread is None:
                self.thread = threading.Thread(target=self._run, daemon=True)
                self.thread.start()
            if self.mqtt is None:
                self._mqtt_start()

    # ---- MQTT (보드가 랜선으로 보냄). 한 번 붙으면 계속 받음, 끊기면 paho 가 다시 붙음 ----

    def _mqtt_start(self):
        try:
            import paho.mqtt.client as paho

            cred = {}
            with open(MQTT_CRED, encoding="utf-8") as f:
                for ln in f:
                    k, _, v = ln.strip().partition("=")
                    cred[k] = v
            c = paho.Client(paho.CallbackAPIVersion.VERSION2, client_id=f"bench-mcp-{os.getpid()}")
            c.username_pw_set(cred.get("user"), cred.get("password"))
            c.on_connect = self._mqtt_connected
            c.on_disconnect = self._mqtt_lost
            c.on_message = self._mqtt_message
            c.reconnect_delay_set(1, 10)
            c.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
            c.loop_start()
            self.mqtt = c
        except Exception as e:  # 계정 파일 없음, paho 없음 등: 시리얼로만 받음
            self.mqtt = False
            self.mqtt_error = f"MQTT 못 씀: {e}"

    def _mqtt_connected(self, c, userdata, flags, rc, props=None):
        with self.lock:
            if rc == 0:
                self.mqtt_ok, self.mqtt_error = True, None
            else:
                self.mqtt_ok, self.mqtt_error = False, f"브로커 {MQTT_HOST}:{MQTT_PORT} 연결 거부: {rc}"
        if rc == 0:
            c.subscribe([(f"{MQTT_TOPIC}/line", 0), (f"{MQTT_TOPIC}/status", 0), (f"{MQTT_TOPIC}/usb", 0)])

    def _mqtt_lost(self, c, userdata, flags, rc, props=None):
        with self.lock:
            self.mqtt_ok = False
            self.mqtt_error = f"브로커 {MQTT_HOST}:{MQTT_PORT} 연결 끊김 ({rc})"

    def _mqtt_message(self, c, userdata, msg):
        text = msg.payload.decode("utf-8", "replace")
        if msg.topic.endswith("/status"):
            with self.lock:
                self.board = text
            return
        if msg.topic.endswith("/usb"):  # 포트를 잡은 프로그램이 시리얼에서 읽어 다시 보낸 줄
            with self.lock:
                self.last_usb_ms = int(time.time() * 1000)
            self._line(text, "serial")
            return
        with self.lock:
            self.last_mqtt_ms = int(time.time() * 1000)
        self._line(text, "mqtt")

    def problem(self) -> str | None:
        """줄이 최근에 들어오고 있으면 None. 아니면 왜 안 들어오는지 (잠금을 잡은 채로 부름)."""
        if self.buf and time.time() * 1000 - self.buf[-1]["t"] < FRESH_MS:
            return None
        parts = []
        if self.board and self.board.startswith("offline"):
            parts.append("보드가 MQTT 에서 끊김 (전원·랜선 확인)")
        elif not self.mqtt_ok:
            parts.append(self.mqtt_error or "MQTT 브로커에 붙는 중")
        if self.error:
            parts.append(self.error)
        return " / ".join(parts) or "측정 줄이 안 들어옴 (펌웨어가 측정 루프에 있는지 확인)"

    def _idle(self) -> bool:
        return time.time() - self.last_call > IDLE_RELEASE_S

    def _idle_exit(self) -> bool:
        # 끝낼지 판단과 thread=None 을 한 잠금 안에서 해야 touch() 와 엇갈리지 않음
        with self.lock:
            if self._idle():
                self.thread = None
                return True
            return False

    def _port_taken(self) -> bool:
        """다른 프로그램이 포트를 잡고 줄을 넘겨 주는 중이거나, 쉼 파일이 있으면 포트를 안 엶."""
        if os.path.exists(SERIAL_PAUSE):
            return True
        with self.lock:
            return time.time() * 1000 - self.last_usb_ms < USB_FRESH_MS

    def _run(self):
        while not self._idle_exit():
            if self._port_taken():
                with self.lock:
                    self.error = None
                time.sleep(1)
                continue
            try:
                s = serial.Serial(PORT, BAUD, timeout=0.2)
            except serial.SerialException as e:
                with self.lock:
                    self.port_open = False
                    self.error = f"{PORT} 를 못 엶 (다른 프로그램이 쓰는 중이거나 USB 빠짐): {e}"
                    self.cond.notify_all()
                time.sleep(2)
                continue
            with self.lock:
                self.port_open = True
                self.error = None
            text, first = "", True
            try:
                while True:
                    with self.lock:
                        if self._idle():
                            break
                    if os.path.exists(SERIAL_PAUSE):
                        break
                    chunk = s.read(512)
                    if not chunk:
                        continue
                    text += chunk.decode("utf-8", "replace")
                    *lines, text = text.split("\n")
                    for raw in lines:
                        if first:  # 포트를 연 순간의 첫 줄은 중간부터라 버림
                            first = False
                            continue
                        self._line(raw, "serial", relay=True)
            except serial.SerialException as e:
                with self.lock:
                    self.error = f"{PORT} 읽다가 끊김: {e}"
            finally:
                s.close()
                with self.lock:
                    self.port_open = False
            time.sleep(0.5)

    def _line(self, raw: str, source: str, relay: bool = False):
        """relay=True: 이 서버가 직접 시리얼에서 읽은 줄. 새 측정 줄이면 <토픽>/usb 로 다시 보냄 (수집기가 받아 적음)."""
        clean = ANSI.sub("", raw).replace("uart:~$ ", "").strip()
        if not clean:
            return
        if clean.startswith("[hsc "):  # HSC 0.1초 값 줄은 수집기(collector.py)가 따로 남김
            return
        m = LINE.search(clean)
        now = int(time.time() * 1000)
        with self.lock:
            if not m:
                if "영점" in clean:
                    self.note = clean
                if source == "serial":
                    _board_log(clean)
                return
            seq = int(m.group(1))
            seg = {}
            for part in SEG.split(m.group(2)):
                name, _, rest = part.strip().partition(" ")
                seg.setdefault(name, rest.strip())
            # USB 와 랜선이 둘 다 꽂혀 있으면 같은 줄이 두 번 옴: 최근 몇 줄 중 같은 번호면 버림
            for r in list(self.buf)[-5:]:
                if r["seq"] == seq and now - r["t"] < 3000:
                    return
            t = TIMING.search(clean)
            period_ms = int(t.group(1)) if t else None
            if period_ms:
                self.period_ms = period_ms
            row = {
                "id": self.next_id,
                "seq": seq,
                "src": source,
                "t": now,
                "period_ms": period_ms,
                "read_us": int(t.group(2)) if t else None,
                "hsc": _sensor(seg["HSC"]),
                "abp2": _sensor(seg["ABP2"]),
                "sdp": _sensor(seg["SDP810"]) if seg.get("SDP810") else None,  # 펌웨어가 SDP810 을 안 읽으면 없음
                "bme": _env(seg["BME280"]) if seg.get("BME280") else None,     # 대기압 · 주변 온도 · 습도 (1초마다 갱신)
                "ds": _env(seg["DS18B20"]) if seg.get("DS18B20") else None,    # 관 속 가스 온도 (1초마다 갱신)
                "raw": clean,
            }
            if venturi:
                row["flow"] = venturi.flows(_venturi_cfg(), row)
            self.buf.append(row)
            self.next_id += 1
            self.cond.notify_all()
            pub = self.mqtt if relay and self.mqtt and self.mqtt_ok else None
        if pub:
            try:
                pub.publish(f"{MQTT_TOPIC}/usb", clean, qos=0)
            except Exception:
                pass  # 브로커가 막 끊긴 순간. 표시는 계속
        sample_log.write(row)

    def wait_for(self, pred, timeout: float) -> bool:
        """pred() 가 참이 되거나 timeout 이 지날 때까지 기다림 (잠금을 잡은 채로 부름)."""
        end = time.time() + timeout
        while not pred():
            left = end - time.time()
            if left <= 0:
                return False
            self.cond.wait(left)
        return True


reader = Reader()


def _source_label(row, board) -> str:
    if row and row.get("src") == "mqtt":
        ip = board.split()[1] if board and board.startswith("online ") and len(board.split()) > 1 else "?"
        return f"랜선 MQTT (보드 {ip})"
    return PORT


def _first_wait_s() -> float:
    # 포트를 막 열었으면 다음 줄까지 측정 주기만큼 걸릴 수 있음 (최대 62초)
    return max(3.0, min((reader.period_ms or 1000) / 1000 + 2, 62))


@mcp.tool(annotations=ToolAnnotations(readOnlyHint=True))
def get_sensors() -> dict:
    """브레드보드 센서 가장 최근 값: HSC(차압, I2C1)와 ABP2(정압, I2C4)의 Pa, °C, 상태.
    각 센서는 {ok, state, pa, c} 이고 ABP2 는 status(상태 바이트)도 있음."""
    reader.touch()
    with reader.lock:
        def fresh():
            if not reader.buf:
                return False
            age = time.time() * 1000 - reader.buf[-1]["t"]
            return age < (reader.period_ms or 1000) + 1500

        reader.wait_for(fresh, _first_wait_s())
        if not reader.buf:
            raise RuntimeError(reader.problem())
        last = dict(reader.buf[-1])
        board = reader.board
    last["port"] = _source_label(last, board)
    last["read_at_ms"] = last["t"]
    return last


def _totals():
    """적산(누적 유량)은 수집기(collector.py)가 셈. 수집기가 1초마다 쓰는 latest.json 에서 읽어 옴.
    수집기가 안 돌거나 10초 넘게 갱신이 없으면 None (왜 그런지는 _logger_state 가 따로 알려 줌)."""
    path = os.path.join(LOG_DIR, "latest.json")
    try:
        if time.time() - os.path.getmtime(path) > 10:
            return None
        with open(path, encoding="utf-8") as f:
            return json.load(f).get("totals")
    except (OSError, ValueError):
        return None


def _logger_state(totals) -> dict:
    """수집기 기록이 멈췄을 때 이유 (reader.lock 을 잡은 채로 부름).
    수집기는 줄이 와야 latest.json 을 씀. 그래서 보드가 멈추거나 랜선이 빠져도 '꺼짐'처럼 보임. 그걸 가려 줌.
      board     : 보드가 아무 길로도 줄을 안 보냄 (멈춤 · 재부팅 · 전원)
      lan       : USB 로는 줄이 오는데 랜선(MQTT)으로는 안 옴, 그리고 수집기 기록도 안 됨
                  (수집기가 시리얼 대체 입력(BENCH_SERIAL_PORT) 없이 돌거나 꺼져 있을 때. 보통은 USB 줄도 수집기가 적음)
      broker    : USB 로는 오는데 이 서버도 브로커에 못 붙음 (Mosquitto 꺼짐 등)
      collector : 랜선으로 줄이 잘 오는데 latest.json 이 안 바뀜 → 수집기 자체가 꺼짐"""
    if totals is not None:
        return {"ok": True, "why": None, "text": None}
    now = time.time() * 1000
    last = reader.buf[-1]["t"] if reader.buf else 0
    if now - reader.last_mqtt_ms < 10000:
        why, text = "collector", "수집기 꺼짐"
    elif now - last < 10000:
        why, text = ("lan", "랜선 끊김 · 기록 안 됨") if reader.mqtt_ok else ("broker", "브로커 끊김 · 기록 안 됨")
    else:
        why, text = "board", "보드 응답 없음"
    return {"ok": False, "why": why, "text": text}


@mcp.tool(annotations=ToolAnnotations(readOnlyHint=True))
def get_history(after_id: int = 0, limit: int = 300) -> dict:
    """측정 기록: after_id 보다 뒤에 쌓인 줄들을 오래된 것부터 돌려줌 (최대 limit 개, 최근 것 우선).
    처음엔 after_id=0, 다음부터는 받은 latest_id 를 넘기면 그 사이 줄만 옴.
    latest_id 가 넘긴 after_id 보다 작으면 서버가 새로 켜진 것이라 처음부터 다시 줌.
    각 줄은 {id, seq, t(받은 시각 ms), period_ms, read_us, hsc, abp2, sdp, bme, ds, flow}.
    bme = BME280 {ok, state, pa(대기압), c(주변 온도), rh(습도 %)}, ds = DS18B20 {ok, state, c(가스 온도)}.
    flow.p_atm · flow.t 는 유량 계산에 실제로 쓴 대기압 · 가스 온도와 출처."""
    reader.touch()
    limit = max(1, min(int(limit), HIST_MAX))
    with reader.lock:
        if not reader.buf:
            reader.wait_for(lambda: bool(reader.buf), _first_wait_s())
        latest = reader.buf[-1] if reader.buf else None
        after = after_id if latest is None or after_id <= latest["id"] else 0
        rows = [r for r in reader.buf if r["id"] > after][-limit:]
        totals = _totals()
        return {
            "port": _source_label(latest, reader.board),
            "port_open": reader.port_open or reader.mqtt_ok,
            "mqtt": {"ok": reader.mqtt_ok, "broker": f"{MQTT_HOST}:{MQTT_PORT}", "board": reader.board},
            "error": reader.problem(),
            "note": reader.note,
            "latest_id": latest["id"] if latest else 0,
            "period_ms": reader.period_ms,
            "totals": totals,
            "logger": _logger_state(totals),
            "cal": venturi.cal_summary(_venturi_cfg()) if venturi else None,  # 지금 쓰는 유출계수 식
            "raw": latest["raw"] if latest else None,
            "samples": [{k: v for k, v in r.items() if k != "raw"} for r in rows],
        }


@mcp.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=True))
def set_c_re(sensor: str, model: str, a: float, b: float, re_min: float, re_max: float = 0, note: str = "") -> dict:
    """유출계수 식(교정값)을 바꿈. 실시간 페이지의 '교정' 에서 계량기 비교 시험 점들로 맞춘 값을 적용할 때 부름.
    sensor: hsc | sdp.  model: inv (C = a + b ÷ Re) | log (C = a + b × log10 Re).  re_min 아래 · re_max 위(0 이면 없음)는 끝 값.
    venturi_cal.json 에 쓰고(이전 값은 history 에 남음), 수집기와 이 서버가 1초 안에 다시 읽어 그 뒤 줄부터 새 식으로 계산함.
    C 가 0.5 ~ 1.05 를 벗어나는 값은 거절. 돌려줌: 지금 쓰는 식 {C, C_re, at}."""
    if not venturi:
        raise RuntimeError("venturi.py 를 못 찾아 유량 계산이 꺼져 있음")
    m = {"model": model, "a": a, "b": b, "re_min": re_min}
    if re_max and re_max > 0:
        m["re_max"] = re_max
    try:
        venturi.save_cal(sensor, m, note)
    except ValueError as e:
        raise RuntimeError(f"교정값 거절: {e}")
    global VENTURI
    VENTURI = venturi.load_config(VENTURI.get("_path"))
    return venturi.cal_summary(VENTURI)


# ---- 지난 기록: 수집기가 남긴 CSV(LOG_DIR\날짜\시.csv) 를 구간 · 간격별 평균으로 돌려줌 ----
# 메모리 버퍼(get_history)는 최근 6분뿐이지만, 디스크에는 수집기가 켜져 있던 동안의 0.1초 줄이 다 있음.
# 시간 파일 하나(약 5 MB, 3만 6천 줄)를 1초 칸으로 줄여 두고(다 쓴 파일은 기억해 둠) 거기서 다시 묶음.

RANGE_MAX_ROWS = 4000  # 한 번에 돌려주는 줄 수. 더 있으면 next_ms 부터 다시 부름 (응답이 너무 크면 앱 다리를 못 넘음)
_SEC_COLS = ("abp2", "hsc", "sdp", "tc", "fh", "fs", "fhn", "fsn", "patm", "ta", "rh")
_hour_cache = collections.OrderedDict()  # path -> (mtime, size, {sec: [n, sums..., counts...]})


def _num(s):
    try:
        return float(s) if s not in (None, "") else None
    except ValueError:
        return None


_hour_lock = threading.Lock()


def _hour_seconds(path):
    """시간 CSV 하나를 1초 칸으로: {epoch_sec: [n, abp2_sum, abp2_n, hsc_sum, hsc_n, ..., 각 칸 합 · 개수]}."""
    with _hour_lock:
        return _hour_seconds_locked(path)


def _hour_seconds_locked(path):
    st = os.stat(path)
    hit = _hour_cache.get(path)
    if hit and hit[0] == st.st_mtime and hit[1] == st.st_size:
        _hour_cache.move_to_end(path)
        return hit[2]
    secs = {}
    with open(path, encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            ts = r.get("time")
            if not ts:
                continue
            try:
                t = time.mktime(time.strptime(ts[:19], "%Y-%m-%dT%H:%M:%S"))
            except ValueError:
                continue
            b = secs.get(t)
            if b is None:
                b = secs[t] = [0] + [0.0, 0] * len(_SEC_COLS)
            b[0] += 1
            ok = {"abp2": r.get("abp2_state") == "정상", "hsc": r.get("hsc_state") == "정상", "sdp": r.get("sdp_state") == "정상",
                  "bme": r.get("bme_state") == "정상", "ds": r.get("ds_state") == "정상"}  # bme · ds 열은 2026-11-20 밤부터
            vals = (
                _num(r.get("abp2_pa")) if ok["abp2"] else None,
                _num(r.get("hsc_pa")) if ok["hsc"] else None,
                _num(r.get("sdp_pa")) if ok["sdp"] else None,
                _num(r.get("ds_c")) if ok["ds"] else _num(r.get("sdp_c")) if ok["sdp"] else _num(r.get("hsc_c")),
                _num(r.get("flow_hsc_m3h")), _num(r.get("flow_sdp_m3h")),
                _num(r.get("flow_hsc_nm3h")), _num(r.get("flow_sdp_nm3h")),
                _num(r.get("bme_pa")) if ok["bme"] else None,
                _num(r.get("bme_c")) if ok["bme"] else None,
                _num(r.get("bme_rh")) if ok["bme"] else None,
            )
            for i, v in enumerate(vals):
                if v is not None:
                    b[1 + 2 * i] += v
                    b[2 + 2 * i] += 1
    if time.time() - st.st_mtime > 120:  # 다 쓴 파일만 기억 (지금 쓰는 파일은 매번 새로 읽음)
        _hour_cache[path] = (st.st_mtime, st.st_size, secs)
        while len(_hour_cache) > 72:
            _hour_cache.popitem(last=False)
    return secs


def _log_files():
    """LOG_DIR 아래 (시작 epoch, 경로) 목록. 'HH.csv' 와 예전 형식 'HH_old….csv' 모두."""
    out = []
    try:
        days = sorted(d for d in os.listdir(LOG_DIR) if re.fullmatch(r"\d{4}-\d{2}-\d{2}", d))
    except OSError:
        return out
    for d in days:
        for fn in sorted(os.listdir(os.path.join(LOG_DIR, d))):
            m = re.fullmatch(r"(\d{2})(?:_old\d+)?\.csv", fn)
            if m:
                t0 = time.mktime(time.strptime(f"{d} {m.group(1)}", "%Y-%m-%d %H"))
                out.append((t0, os.path.join(LOG_DIR, d, fn)))
    return out


@mcp.tool(annotations=ToolAnnotations(readOnlyHint=True))
def get_range(start_ms: int = 0, end_ms: int = 0, step_s: int = 1) -> dict:
    """지난 기록(수집기 CSV)을 step_s 초 칸 평균으로 돌려줌. start_ms ~ end_ms (epoch ms, 0 이면 처음 · 지금).
    각 줄은 cols 순서의 배열: t_ms(칸 시작), n(줄 수), abp2_pa, hsc_pa, sdp_pa,
    temp_c(가스 온도: DS18B20, 없으면 SDP810, 그것도 없으면 HSC 칩 온도),
    flow_hsc_m3h, flow_sdp_m3h, flow_hsc_nm3h, flow_sdp_nm3h,
    p_atm_pa(BME280 대기압), amb_c(BME280 주변 온도), rh_pct(BME280 습도) (평균, 값 없으면 null).
    한 번에 최대 4000줄. 더 있으면 next_ms 가 오고, 그걸 start_ms 로 다시 부르면 이어짐.
    available 은 디스크에 있는 기록의 처음 · 마지막 파일 시각."""
    step = max(1, int(step_s))
    files = _log_files()
    now = time.time()
    t_start = start_ms / 1000 if start_ms else (files[0][0] if files else now)
    t_end = end_ms / 1000 if end_ms else now
    t_start = t_start - (t_start % step)
    rows, next_ms = [], None
    cur, acc = None, None

    def flush():
        if cur is None or not acc[0]:
            return
        row = [int(cur * 1000), acc[0]]
        for i in range(len(_SEC_COLS)):
            s, n = acc[1 + 2 * i], acc[2 + 2 * i]
            row.append(round(s / n, 5) if n else None)
        rows.append(row)

    hours = collections.OrderedDict()  # 같은 시간 파일이 둘(예전 형식 HH_old….csv + HH.csv)이면 합침
    for f0, path in files:
        if f0 + 3600 <= t_start or f0 >= t_end:
            continue
        hours.setdefault(f0, []).append(path)
    for f0, paths in hours.items():
        secs = {}
        for path in paths:
            try:
                part = _hour_seconds(path)
            except OSError:
                continue
            for t, b in part.items():
                if t in secs:
                    secs[t] = [x + y for x, y in zip(secs[t], b)]
                else:
                    secs[t] = b
        for t in sorted(secs):
            if t < t_start or t >= t_end:
                continue
            k = t - (t % step)
            if k != cur:
                flush()
                if len(rows) >= RANGE_MAX_ROWS:
                    next_ms = int(k * 1000)
                    break
                cur, acc = k, [0] + [0.0, 0] * len(_SEC_COLS)
            b = secs[t]
            for i in range(len(b)):
                acc[i] += b[i]
        if next_ms is not None:
            break
    if next_ms is None:
        flush()
    return {
        "step_s": step,
        "cols": ["t_ms", "n", "abp2_pa", "hsc_pa", "sdp_pa", "temp_c", "flow_hsc_m3h", "flow_sdp_m3h", "flow_hsc_nm3h", "flow_sdp_nm3h",
                 "p_atm_pa", "amb_c", "rh_pct"],
        "rows": rows,
        "next_ms": next_ms,
        "available": {"first_ms": int(files[0][0] * 1000) if files else None, "last_ms": int(now * 1000) if files else None},
        "period_ms": reader.period_ms,
    }


if __name__ == "__main__":
    # 페이지가 안 열려 있어도 랜선(MQTT)으로 오는 줄은 받아서 logs 에 남김. COM 포트는 툴이 불릴 때만 엶.
    reader._mqtt_start()
    mcp.run()
