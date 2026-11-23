"""STM32 센서 수집기: MQTT 로 오는 보드 측정 줄을 받아 CSV · JSON 으로 남김.

윈도우 · 맥 · 리눅스에서 그대로 돎. Claude 앱이나 MCP 없이 혼자 도는 프로그램임.

  보드(NUCLEO-H563ZI) --랜선 MQTT--> 브로커(Mosquitto) --> 이 수집기 --> logs/날짜/시.csv
                     \\--USB 시리얼 (랜선으로 안 올 때만) ------------^

실행:
  pip install paho-mqtt
  python collector.py

설정은 환경변수로 바꿈 (괄호 안이 기본값):
  BENCH_MQTT_HOST   브로커 주소 (127.0.0.1)
  BENCH_MQTT_PORT   브로커 포트 (1883)
  BENCH_MQTT_TOPIC  토픽 앞부분 (h563/bench). 뒤에 /line, /status 가 붙음
  BENCH_MQTT_CRED   계정 파일 (이 파일 옆의 credentials.txt). 내용은 user=... / password=... 두 줄
  BENCH_LOG_DIR     기록 폴더 (이 파일 옆의 logs)
  BENCH_FLUSH_S     파일에 쓰는 간격 초 (1). 보드가 0.1초마다 보내면 1초마다 10줄씩 한꺼번에 씀
  BENCH_JSONL       1 이면 시간별 JSON(.jsonl)도 남김 (0). 기본은 CSV 만. latest.json 은 항상 남음
  BENCH_VENTURI     벤투리 설정 파일 (이 파일 옆의 venturi.json). 치수 · 가스 · 대기압
  BENCH_SERIAL_PORT 보드 USB 시리얼 포트 (비어 있음 = 안 씀). 윈도우 COM3, 맥 /dev/tty.usbmodem...  pip install pyserial
  BENCH_SERIAL_AFTER_S  줄이 이만큼(초) 안 오면 시리얼을 엶 (5)

시리얼 대체 입력: 랜선이 빠져 MQTT 로 줄이 안 오면, 보드 USB 가 이 PC 에 꽂혀 있을 때 시리얼에서 같은 줄을 읽어
그대로 기록함 (CSV 의 src 열이 serial). 읽은 측정 줄은 브로커의 <토픽>/usb 로도 다시 보내서, 포트를 못 여는
다른 프로그램(Claude 페이지 서버)도 값을 봄. 반대로 다른 프로그램이 포트를 잡고 <토픽>/usb 로 보내 주면 그걸 받아 적음.
랜선으로 30초 동안 다시 잘 오면 포트를 놓음. <기록 폴더>/serial.pause 파일이 있는 동안은 포트를 안 잡음
(시리얼 터미널을 써야 할 때 그 파일을 만들고, 끝나면 지움).

적산: 줄마다 유량 × 측정 주기를 더해 누적 부피를 셈 (Totals). CSV 끝 4 열(total_hsc_m3, total_hsc_nm3,
total_sdp_m3, total_sdp_nm3)이 since 부터의 누적이고, latest.json 의 "totals" 에 오늘 누적도 있음.
<기록 폴더>/totals.json 에 저장되어 껐다 켜도 이어짐. 처음부터 세려면 끄고 totals.json 삭제.

유량: 줄마다 SDP810 차압과 HSC 차압으로 각각 벤투리 유량(m³/h)과 표준상태 유량(Nm³/h)을
계산해 CSV 끝 열(rho_kgm3, flow_sdp_*, flow_hsc_*)과 JSON 의 "flow" 에 넣음 (venturi.py).
밀도에는 BME280 대기압(bme_pa)과 DS18B20 가스 온도(ds_c)를 씀. 둘 중 없는 것은 venturi.json 의
p_atm_pa · 다른 센서 칩 온도로 대신하고, 실제로 쓴 값과 출처는 JSON 의 flow.p_atm · flow.t 에 남음.

기록:
  <BENCH_LOG_DIR>/2026-11-18/12.csv     한 시간에 파일 하나. 엑셀용 (첫 줄이 열 이름, UTF-8 BOM)
  <BENCH_LOG_DIR>/2026-11-18/12.jsonl   (BENCH_JSONL=1 일 때만) 한 줄에 측정 하나씩 JSON, 보드 원문 raw 포함
  <BENCH_LOG_DIR>/2026-11-18/12_hsc.csv HSC 만 0.1초마다 (1초에 10줄씩 한꺼번에 덧붙임)
  <BENCH_LOG_DIR>/board.log             측정 줄이 아닌 보드 안내 (영점, I2C 스캔, 접속/끊김)
  <BENCH_LOG_DIR>/latest.json           가장 최근 측정 하나 (웹 페이지가 읽기 쉬우라고 매번 덮어씀)

같은 기록 폴더에 수집기를 두 개 띄워도 하나만 씀 (.writer.lock).
"""

import collections
import csv
import json
import os
import re
import sys
import threading
import time

import paho.mqtt.client as paho

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import venturi  # noqa: E402  같은 폴더의 벤투리 유량 계산

HERE = os.path.dirname(os.path.abspath(__file__))
MQTT_HOST = os.environ.get("BENCH_MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("BENCH_MQTT_PORT", "1883"))
MQTT_TOPIC = os.environ.get("BENCH_MQTT_TOPIC", "h563/bench")
MQTT_CRED = os.environ.get("BENCH_MQTT_CRED", os.path.join(HERE, "credentials.txt"))
LOG_DIR = os.environ.get("BENCH_LOG_DIR", os.path.join(HERE, "logs"))
WRITE_JSONL = os.environ.get("BENCH_JSONL", "0") == "1"  # 시간별 JSON 기록도 남길지 (기본은 CSV 만)
FLUSH_S = float(os.environ.get("BENCH_FLUSH_S", "1"))  # 모아 둔 측정 줄을 파일에 쓰는 간격 (초)
SERIAL_PORT = os.environ.get("BENCH_SERIAL_PORT", "")  # 보드 USB 시리얼 포트. 비우면 시리얼 대체 입력을 안 씀
SERIAL_BAUD = int(os.environ.get("BENCH_SERIAL_BAUD", "115200"))
SERIAL_AFTER_S = float(os.environ.get("BENCH_SERIAL_AFTER_S", "5"))  # 줄이 (어느 길로든) 이만큼 안 오면 시리얼을 엶
SERIAL_LAN_BACK_S = 30  # 랜선으로 이만큼 계속 잘 오면 시리얼을 놓음
SERIAL_PAUSE = os.path.join(LOG_DIR, "serial.pause")  # 이 파일이 있는 동안은 포트를 안 잡음
NOTE_REPEAT_S = 600  # 시리얼의 "주소 없음" 안내(랜선 없으면 10초마다 나옴)는 이 간격으로만 board.log 에 남김
# 그 안내 줄. 보드가 측정 줄과 겹쳐 찍으면 앞머리가 잘려 "선 링크 안 붙음 ..., MAC ..." 처럼 뒷부분만 올 때도 있음
NET_WAIT = re.compile(r"주소 없음|링크 (안 )?붙음|포트 확인\)|MAC [0-9A-Fa-f]{2}:")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")  # 시리얼 셸이 붙이는 색 코드

# 보드가 보내는 측정 줄 ("  |  " 로 나뉜 칸마다 센서 하나. HSC · ABP2 는 꼭 있고 나머지는 펌웨어에 따라 있음):
# [period 100 ms, read 14115 us] #198  HSC 정상 -0.21 Pa 28.3 C  |  ABP2 정상 20.4 Pa 24.97 C (st 0x40)
#   |  SDP810 정상 -0.05 Pa 25.7 C  |  BME280 정상 102039.6 Pa 23.92 C 21.5 %  |  DS18B20 정상 24.31 C
# BME280(대기압 · 주변 온도 · 습도)과 DS18B20(관 속 가스 온도)은 보드가 1초마다 읽고, 그 값을 줄마다 다시 붙임.
# 모르는 이름의 칸은 건너뜀 (펌웨어가 센서를 더 붙여도 수집이 안 끊기게).
LINE = re.compile(r"#(\d+)\s+(HSC\s+.+?\s+\|\s+ABP2\s+.+?)\s*$")
SEG = re.compile(r"\s+\|\s+")
TIMING = re.compile(r"\[period\s+(\d+)\s+ms,\s+read\s+(\d+)\s+us\]")
VALS = re.compile(r"^(\S+(?:\s\S+)*?)(?:\s+(-?[\d.]+)\s+Pa\s+(-?[\d.]+)\s+C)?(?:\s+\(st\s+(0x[0-9a-fA-F]+)\))?$")
# 환경 센서 칸: "정상 102039.6 Pa 23.92 C 21.5 %" (BME280) · "정상 24.31 C" (DS18B20) · "응답 없음"
ENV = re.compile(r"^(\S+(?:\s\S+)*?)(?:\s+(-?[\d.]+)\s+Pa)?(?:\s+(-?[\d.]+)\s+C)?(?:\s+(-?[\d.]+)\s+%)?$")

# HSC 0.1초 값 줄 (측정 줄 바로 뒤에 1초마다 하나): [hsc 100 ms] #198  0.09 0.24 x 0.09 ...
FAST = re.compile(r"^\[hsc\s+(\d+)\s+ms\]\s+#(\d+)\s+(.*)$")
HSC_HEAD = ["time", "seq", "idx", "hsc_pa"]

CSV_HEAD = ["time", "seq", "src", "period_ms", "read_us",
            "hsc_state", "hsc_pa", "hsc_c", "abp2_state", "abp2_pa", "abp2_c", "abp2_status",
            "sdp_state", "sdp_pa", "sdp_c",
            "bme_state", "bme_pa", "bme_c", "bme_rh", "ds_state", "ds_c",
            "rho_kgm3", "flow_sdp_m3h", "flow_sdp_nm3h", "flow_hsc_m3h", "flow_hsc_nm3h",
            "total_hsc_m3", "total_hsc_nm3", "total_sdp_m3", "total_sdp_nm3"]

VENTURI = venturi.load_config()  # 벤투리 치수 · 가스 · 대기압 (venturi.json) + 교정값 (venturi_cal.json). 바뀌면 flush 때 다시 읽음


def parse_sensor(text):
    """'정상 -0.21 Pa 28.3 C' -> {ok, state, pa, c[, status]}. 값이 없으면 pa · c 는 None."""
    m = VALS.match(text.strip())
    if not m:
        return {"ok": False, "state": text.strip(), "pa": None, "c": None}
    state, pa, c, st = m.groups()
    out = {"ok": state == "정상", "state": state,
           "pa": float(pa) if pa is not None else None,
           "c": float(c) if c is not None else None}
    if st is not None:
        out["status"] = st
    return out


def parse_env(text):
    """'정상 102039.6 Pa 23.92 C 21.5 %' -> {ok, state, pa, c, rh}. 없는 값은 None (DS18B20 은 c 만 있음)."""
    m = ENV.match(text.strip())
    if not m:
        return {"ok": False, "state": text.strip(), "pa": None, "c": None, "rh": None}
    state, pa, c, rh = m.groups()
    f = lambda v: float(v) if v is not None else None  # noqa: E731
    return {"ok": state == "정상", "state": state, "pa": f(pa), "c": f(c), "rh": f(rh)}


def parse_line(text, now_ms):
    """측정 줄이면 dict, 아니면 None."""
    m = LINE.search(text)
    if not m:
        return None
    seg = {}
    for part in SEG.split(m.group(2)):
        name, _, rest = part.strip().partition(" ")
        seg.setdefault(name, rest.strip())
    t = TIMING.search(text)
    return {
        "seq": int(m.group(1)),
        "src": "mqtt",
        "t": now_ms,
        "period_ms": int(t.group(1)) if t else None,
        "read_us": int(t.group(2)) if t else None,
        "hsc": parse_sensor(seg["HSC"]),
        "abp2": parse_sensor(seg["ABP2"]),
        "sdp": parse_sensor(seg["SDP810"]) if seg.get("SDP810") else None,
        "bme": parse_env(seg["BME280"]) if seg.get("BME280") else None,   # 예전 펌웨어 줄에는 없음
        "ds": parse_env(seg["DS18B20"]) if seg.get("DS18B20") else None,
        "raw": text,
    }


def replace_file(tmp, dst):
    """tmp 를 dst 로 바꿔치기. 윈도우에서는 다른 프로그램(MCP 서버 · 백신 등)이 dst 를 읽는 순간
    os.replace 가 PermissionError 로 실패함. 몇 번 다시 해 보고, 그래도 안 되면 이번은 건너뜀
    (1초 뒤 다음 묶음 때 새 값으로 다시 씀). 수집기가 이것 때문에 죽으면 안 됨."""
    for _ in range(10):
        try:
            os.replace(tmp, dst)
            return True
        except PermissionError:
            time.sleep(0.02)
    return False


class Totals:
    """적산(누적 유량). 측정 줄마다 유량 × 측정 주기(period_ms)를 더함.

    두 차압 센서(HSC · SDP810) 각각, 실제 부피(m³)와 표준상태 부피(Nm³)를 따로 셈.
    'total' 은 since 부터 계속 쌓이고, 'today' 는 측정 시각의 날짜가 바뀌면 0 부터 다시 셈.
    값은 <기록 폴더>/totals.json 에 1초마다 저장해서 수집기를 껐다 켜도 이어짐.
    처음부터 다시 세려면 수집기를 끄고 totals.json 을 지우면 됨.
    유량 값이 없는 줄(센서 오류)은 더하지 않음. 보드가 꺼져 있던 동안은 줄이 없으니 더할 것도 없음."""

    def __init__(self, path):
        self.path = path
        try:
            with open(path, encoding="utf-8") as f:
                self.d = json.load(f)
        except (OSError, ValueError):
            self.d = {"since": time.strftime("%Y-%m-%dT%H:%M:%S"), "day": None,
                      "total": {k: {"m3": 0.0, "nm3": 0.0} for k in ("hsc", "sdp")},
                      "today": {k: {"m3": 0.0, "nm3": 0.0} for k in ("hsc", "sdp")}}

    def add(self, r, fl):
        day = time.strftime("%Y-%m-%d", time.localtime(r["t"] / 1000))
        if self.d.get("day") != day:
            self.d["day"] = day
            self.d["today"] = {k: {"m3": 0.0, "nm3": 0.0} for k in ("hsc", "sdp")}
        dt_h = (r.get("period_ms") or 0) / 3.6e6
        for k in ("hsc", "sdp"):
            q = fl.get(k) or {}
            if q.get("m3h") is None or dt_h <= 0:
                continue
            for part, key in (("m3", "m3h"), ("nm3", "nm3h")):
                v = q[key] * dt_h
                self.d["total"][k][part] += v
                self.d["today"][k][part] += v
        return self.snapshot()

    def snapshot(self):
        r6 = lambda x: round(x, 6)  # noqa: E731  (1 mL 단위)
        return {"since": self.d["since"], "day": self.d["day"],
                **{k: {"m3": r6(self.d["total"][k]["m3"]), "nm3": r6(self.d["total"][k]["nm3"])} for k in ("hsc", "sdp")},
                "today": {k: {"m3": r6(self.d["today"][k]["m3"]), "nm3": r6(self.d["today"][k]["nm3"])}
                          for k in ("hsc", "sdp")}}

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.d, f, ensure_ascii=False)
        replace_file(tmp, self.path)


class Writer:
    def __init__(self):
        os.makedirs(LOG_DIR, exist_ok=True)
        self.lockf = open(os.path.join(LOG_DIR, ".writer.lock"), "a+")
        self.owner = False
        self.next_try = 0.0
        self.checked = set()
        self.lock = threading.Lock()
        self.pending = []  # 아직 파일에 안 쓴 측정 줄
        self.totals = Totals(os.path.join(LOG_DIR, "totals.json"))

    def own(self):
        """기록 폴더의 잠금을 잡음. 다른 수집기가 쓰는 중이면 30초마다 다시 시도."""
        if self.owner:
            return True
        if time.time() < self.next_try:
            return False
        self.next_try = time.time() + 30
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(self.lockf.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.lockf.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.owner = True
            print(f"기록 시작: {LOG_DIR}", flush=True)
        except OSError:
            print("다른 수집기가 이 폴더에 쓰는 중이라 기다림 (30초마다 다시 봄)", flush=True)
        return self.owner

    def sample(self, r):
        """측정 줄 하나를 모아 둠. 파일에는 flush() 가 FLUSH_S 마다 한꺼번에 씀."""
        with self.lock:
            self.pending.append(r)

    def flush(self):
        with self.lock:
            rows, self.pending = self.pending, []
        if not rows or not self.own():
            return
        global VENTURI
        new_cfg = venturi.refresh(VENTURI)
        if new_cfg is not VENTURI:
            VENTURI = new_cfg
            self.note(f"[설정] 벤투리 설정 다시 읽음: {json.dumps(venturi.cal_summary(VENTURI), ensure_ascii=False)}")
        groups = {}  # 시간별 파일 -> [(csv 값, json 줄)]
        for r in rows:
            ts = time.localtime(r["t"] / 1000)
            day = os.path.join(LOG_DIR, time.strftime("%Y-%m-%d", ts))
            base = os.path.join(day, time.strftime("%H", ts))
            iso = time.strftime("%Y-%m-%dT%H:%M:%S", ts) + f".{r['t'] % 1000:03d}"
            h, a, s = r["hsc"], r["abp2"], r.get("sdp") or {}
            b, d = r.get("bme") or {}, r.get("ds") or {}
            fl = venturi.flows(VENTURI, r)
            tot = self.totals.add(r, fl)
            vals = [iso, r["seq"], r["src"], r["period_ms"], r["read_us"],
                    h["state"], h["pa"], h["c"], a["state"], a["pa"], a["c"], a.get("status"),
                    s.get("state"), s.get("pa"), s.get("c"),
                    b.get("state"), b.get("pa"), b.get("c"), b.get("rh"), d.get("state"), d.get("c"),
                    fl["rho"], fl["sdp"]["m3h"], fl["sdp"]["nm3h"], fl["hsc"]["m3h"], fl["hsc"]["nm3h"],
                    tot["hsc"]["m3"], tot["hsc"]["nm3"], tot["sdp"]["m3"], tot["sdp"]["nm3"]]
            groups.setdefault(base, []).append((vals, {"time": iso, **r, "flow": fl, "totals": tot}))
        for base, items in groups.items():
            os.makedirs(os.path.dirname(base), exist_ok=True)
            csv_path = base + ".csv"
            new = not os.path.exists(csv_path)
            if not new and csv_path not in self.checked:  # 열 구성이 다른 옛 파일이면 옆으로 치움
                with open(csv_path, encoding="utf-8-sig") as f:
                    old = f.readline().strip() != ",".join(CSV_HEAD)
                if old:
                    os.replace(csv_path, base + f"_old{int(time.time())}.csv")
                    new = True
            self.checked.add(csv_path)
            with open(csv_path, "a", encoding="utf-8-sig" if new else "utf-8", newline="") as f:
                w = csv.writer(f)
                if new:
                    w.writerow(CSV_HEAD)
                w.writerows([["" if v is None else v for v in vals] for vals, _ in items])
            if WRITE_JSONL:
                with open(base + ".jsonl", "a", encoding="utf-8") as f:
                    f.write("".join(json.dumps(row, ensure_ascii=False) + "\n" for _, row in items))
        self.totals.save()
        last = groups[list(groups)[-1]][-1][1]
        tmp = os.path.join(LOG_DIR, "latest.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(last, f, ensure_ascii=False)
        replace_file(tmp, os.path.join(LOG_DIR, "latest.json"))

    def hsc_fast(self, seq, step_ms, vals, now_ms):
        """HSC 0.1초 값들을 <날짜>/<시>_hsc.csv 에 한꺼번에 씀. 마지막 값이 받은 시각, 앞은 step_ms 씩 앞당김."""
        if not self.own():
            return
        ts = time.localtime(now_ms / 1000)
        day = os.path.join(LOG_DIR, time.strftime("%Y-%m-%d", ts))
        os.makedirs(day, exist_ok=True)
        path = os.path.join(day, time.strftime("%H", ts) + "_hsc.csv")
        new = not os.path.exists(path)
        rows = []
        for i, v in enumerate(vals):
            t = now_ms - (len(vals) - 1 - i) * step_ms
            iso = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(t / 1000)) + f".{t % 1000:03d}"
            rows.append([iso, seq, i, "" if v is None else v])
        with open(path, "a", encoding="utf-8-sig" if new else "utf-8", newline="") as f:
            w = csv.writer(f)
            if new:
                w.writerow(HSC_HEAD)
            w.writerows(rows)

    def note(self, text):
        if not self.own():
            return
        with open(os.path.join(LOG_DIR, "board.log"), "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {text}\n")


def clean_line(text):
    """시리얼 줄에서 색 코드 · 셸 프롬프트를 뗌 (MQTT 줄은 그대로임)."""
    return ANSI.sub("", text).replace("uart:~$ ", "").strip()


class Intake:
    """보드 줄 하나를 받아 처리함. 랜선(MQTT)으로 오든 USB 시리얼로 오든 같은 길로 들어옴."""

    def __init__(self, writer):
        self.writer = writer
        self.lock = threading.Lock()
        self.recent = collections.deque(maxlen=64)  # 최근 측정 줄 (번호, 받은 시각 ms). 두 길로 같은 줄이 오면 하나만 남김
        self.last_fast = None
        self.last_line = time.time()  # 측정 줄이 어느 길로든 마지막으로 온 시각 (초). 켠 직후엔 SERIAL_AFTER_S 만큼 기다림
        self.last_lan = 0.0           # 그중 랜선으로 마지막으로 온 시각
        self.noted = {}               # 시리얼 "주소 없음" 안내: 링크 상태 -> 마지막으로 남긴 시각 (ms)

    def feed(self, text, src="mqtt", now_ms=None):
        """줄 하나. src 는 "mqtt"(랜선) 또는 "serial"(USB).
        돌려주는 값: "line" 새 측정 줄, "fast" HSC 빠른 줄, "dup" 이미 받은 줄, "note" 안내 줄, None 빈 줄."""
        text = clean_line(text)
        if not text:
            return None
        now = int(time.time() * 1000) if now_ms is None else now_ms
        with self.lock:
            fm = FAST.match(text)
            if fm:
                seq = int(fm.group(2))
                if seq == self.last_fast:
                    return "dup"
                self.last_fast = seq
                vals = [None if v == "x" else float(v) for v in fm.group(3).split()]
                self.writer.hsc_fast(seq, int(fm.group(1)), vals, now)
                return "fast"
            r = parse_line(text, now)
            if r is None:
                if src == "serial" and NET_WAIT.search(text):
                    key = "링크 없음" if "안 붙음" in text or "포트 확인" in text else "링크 있음"  # 상태가 바뀌면 바로 남김
                    if now - self.noted.get(key, -NOTE_REPEAT_S * 1000) < NOTE_REPEAT_S * 1000:
                        return "note"
                    self.noted[key] = now
                self.writer.note(text)
                return "note"
            self.last_line = now / 1000
            if src == "mqtt":
                self.last_lan = now / 1000
            # 같은 줄이 두 번 오면(재접속 직후, 랜선 + USB 둘 다) 하나만 남김
            if any(s == r["seq"] and now - t < 3000 for s, t in self.recent):
                return "dup"
            self.recent.append((r["seq"], now))
            r["src"] = src
            self.writer.sample(r)
            return "line"


def serial_loop(intake, relay):
    """랜선으로 줄이 안 올 때 보드 USB 시리얼(SERIAL_PORT)에서 대신 읽음 (따로 도는 스레드).
    새로 받은 측정 줄은 relay(글)로 브로커에도 다시 보냄."""
    try:
        import serial
    except ImportError:
        print("pyserial 이 없어 시리얼 대체 입력을 안 씀 (pip install pyserial)", flush=True)
        return
    said = [None]

    def say(msg):  # 같은 말은 한 번만
        if msg != said[0]:
            said[0] = msg
            print(msg, flush=True)
            intake.writer.note(f"[수집기] {msg}")

    while True:
        if os.path.exists(SERIAL_PAUSE):
            say(f"시리얼 쉼: {SERIAL_PAUSE} 파일이 있는 동안은 {SERIAL_PORT} 를 안 잡음")
            time.sleep(1)
            continue
        if time.time() - intake.last_line < SERIAL_AFTER_S:  # 다른 길로 줄이 오는 중
            time.sleep(1)
            continue
        try:
            s = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=0.2)
        except (serial.SerialException, OSError):
            say(f"{SERIAL_PORT} 를 못 엶 (USB 빠짐이거나 다른 프로그램이 쓰는 중). 5초마다 다시 봄")
            time.sleep(5)
            continue
        say(f"줄이 {SERIAL_AFTER_S:g}초 넘게 안 와서 {SERIAL_PORT} 시리얼로 받음")
        buf, first, lan_since, why = b"", True, None, ""
        try:
            while True:
                if os.path.exists(SERIAL_PAUSE):
                    why = "serial.pause 파일이 생김"
                    break
                now = time.time()
                if now - intake.last_lan < 2:
                    lan_since = lan_since or now
                    if now - lan_since >= SERIAL_LAN_BACK_S:
                        why = "랜선으로 다시 잘 옴"
                        break
                else:
                    lan_since = None
                buf += s.read(s.in_waiting or 1)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    if first:  # 포트를 연 순간의 첫 줄은 중간부터라 버림
                        first = False
                        continue
                    text = raw.decode("utf-8", "replace")
                    if intake.feed(text, "serial") in ("line", "fast"):
                        relay(clean_line(text))
        except (serial.SerialException, OSError) as e:
            why = f"읽다가 끊김: {e}"
        finally:
            try:
                s.close()
            except Exception:
                pass
        say(f"{SERIAL_PORT} 놓음 ({why})")
        time.sleep(1)


def main():
    cred = {}
    try:
        with open(MQTT_CRED, encoding="utf-8") as f:
            for ln in f:
                k, _, v = ln.strip().partition("=")
                cred[k] = v
    except OSError:
        print(f"계정 파일이 없음: {MQTT_CRED} (user=... / password=... 두 줄). 계정 없이 붙어 봄", flush=True)

    writer = Writer()
    intake = Intake(writer)

    def on_connect(c, userdata, flags, rc, props=None):
        if rc == 0:
            print(f"브로커 {MQTT_HOST}:{MQTT_PORT} 연결됨, {MQTT_TOPIC}/# 받는 중", flush=True)
            c.subscribe([(f"{MQTT_TOPIC}/line", 0), (f"{MQTT_TOPIC}/status", 0), (f"{MQTT_TOPIC}/usb", 0)])
        else:
            print(f"브로커가 연결 거부: {rc} (계정 확인)", flush=True)

    def on_disconnect(c, userdata, flags, rc, props=None):
        print(f"브로커 연결 끊김 ({rc}). 알아서 다시 붙음", flush=True)

    def on_message(c, userdata, msg):
        text = msg.payload.decode("utf-8", "replace").strip()
        if not text:
            return
        if msg.topic.endswith("/status"):
            writer.note(f"[상태] {text}")
            return
        # <토픽>/usb = 누군가 USB 시리얼에서 읽어 다시 보낸 줄, <토픽>/line = 보드가 랜선으로 보낸 줄
        intake.feed(text, "serial" if msg.topic.endswith("/usb") else "mqtt")

    c = paho.Client(paho.CallbackAPIVersion.VERSION2, client_id=f"bench-collector-{os.getpid()}")
    if cred.get("user"):
        c.username_pw_set(cred.get("user"), cred.get("password"))
    c.on_connect, c.on_disconnect, c.on_message = on_connect, on_disconnect, on_message
    c.reconnect_delay_set(1, 10)
    c.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    c.loop_start()
    if SERIAL_PORT:
        relay = lambda text: c.publish(f"{MQTT_TOPIC}/usb", text, qos=0)  # noqa: E731
        threading.Thread(target=serial_loop, args=(intake, relay), daemon=True).start()
        print(f"시리얼 대체 입력 켬: 줄이 {SERIAL_AFTER_S:g}초 안 오면 {SERIAL_PORT} 에서 읽음", flush=True)
    try:
        while True:  # 모아 둔 줄을 FLUSH_S 마다 한꺼번에 파일로
            time.sleep(FLUSH_S)
            try:
                writer.flush()
            except Exception as e:  # 파일 하나가 잠깐 막혀도 수집기는 계속 돎 (다음 묶음에서 다시 씀)
                print(f"기록 중 오류 (계속 돎): {type(e).__name__}: {e}", flush=True)
    except KeyboardInterrupt:
        writer.flush()
        c.loop_stop()
        print("끝", flush=True)
        sys.exit(0)


if __name__ == "__main__":
    main()
