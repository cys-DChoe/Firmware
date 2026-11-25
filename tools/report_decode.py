#!/usr/bin/env python3
"""1분 보고(형식 v2 28바이트 · 옛 v1 24바이트) 풀기 — 서버 쪽 참고 구현.

v2(11/24) = v1 24바이트 + SDP810 차압 1분 평균(0.02 Pa) + DS18B20 온도(0.01 °C), 둘 다 부호 있는 16비트, 0x8000 = 없음.

펌웨어의 Core/Inc/report.h 표와 같은 형식. C 코드와 따로 짰고,
tests/host/run.sh 가 C 가 묶은 값을 이걸로 풀어 맞는지 확인함.

쓰는 법:
    python3 tools/report_decode.py 0101020...      # 16진 문자열 하나 풀기
    python3 tools/report_decode.py --selftest FILE # 시험 벡터 대조
    python3 tools/report_decode.py --counts 01...  # 포트 3 횟수 보고 풀기
    python3 tools/report_decode.py --cal 01...     # 포트 4 교정값 보고 풀기 (11/24)
"""
import struct
import sys

LEN = 28
LEN_V1 = 24
FMT = ">BHIIHhhhHBH"   # 버전 · 순번 · 시각 · 누적 · 유량 · 정압 · 차압 · 온도 · 대기압 · 배터리 · 상태 (앞 24 B)
NONE16 = -32768        # 24 · 26 번 "값 없음"

FLAGS = [
    "FLOWING",        # 조리 중
    "VENTING",        # 방출 중
    "DEPLETED",       # 고갈
    "LEAK_B",         # 계량기 뒤 누설
    "OUT_OF_RANGE",   # 센서 만점 95 % 넘음
    "BATT_WARN",      # 셀 3.5 V 아래
    "BATT_QUALITY",   # 셀 3.35 V 아래
    "ZERO_UNSET",     # 영점 안 잡음
    "TIME_UNSYNCED",  # 시각 믿지 말 것
    "RESENT",         # 다시 보낸 기록
    "CLIPPED",        # 범위 넘어 잘린 값 있음
    "INCOMPLETE",     # 측정 시간 54 s 미만
    "BOOT",           # 켠 뒤 첫 기록
    "FAULT_RESET",    # 워치독 · 오류로 재시작한 뒤 첫 기록
    "BATT_OFF",       # 셀이 낮아 이 기록 뒤 측정 · 보고를 멈춤
    "BARO_HELD",      # 기압을 못 잰 때가 있음 — 마지막 값(처음엔 해수면 값)으로 계산
]


def decode(buf: bytes) -> dict:
    if len(buf) not in (LEN, LEN_V1):
        raise ValueError(f"길이가 {len(buf)} 바이트임 (28 · 옛 24 여야 함)")
    ver, seq, unix_s, total, flow, pg, dp, t, patm, batt, flags = struct.unpack(FMT, buf[:LEN_V1])
    if (ver, len(buf)) not in ((2, LEN), (1, LEN_V1)):
        raise ValueError(f"형식 버전 {ver} 과 길이 {len(buf)} 가 안 맞음")
    sdp = probe = NONE16
    if ver == 2:
        sdp, probe = struct.unpack(">hh", buf[LEN_V1:])
    return {
        "version": ver,
        "seq": seq,
        "unix_s": unix_s,
        "total_nm3": total / 10000.0,
        "flow_nm3h": flow / 1000.0,
        "pg_pa": float(pg),
        "dp_pa": dp / 20.0,
        "t_c": t / 10.0,
        "patm_pa": patm + 50000.0,
        "batt_mv": batt * 10 + 2500,
        "flags": flags,
        "flag_names": [n for i, n in enumerate(FLAGS) if flags & (1 << i)],
        "sdp810_pa": None if sdp == NONE16 else sdp / 50.0,
        "ds18b20_c": None if probe == NONE16 else probe / 100.0,
    }


# 눈금의 절반 = 반올림으로 생길 수 있는 최대 차이
TOL = {"total_nm3": 0.00005, "flow_nm3h": 0.0005, "pg_pa": 0.5, "dp_pa": 0.025,
       "t_c": 0.05, "patm_pa": 0.5, "batt_mv": 5, "sdp810_pa": 0.01, "ds18b20_c": 0.005}


COUNTS_FMT = ">BHIIII"   # 버전 · 1분 보고 순번 · 조리 · 고갈 · 방출 분 · 누설(계량기 뒤)
COUNTS_LEN = struct.calcsize(COUNTS_FMT)


def decode_counts(buf: bytes) -> dict:
    """포트 3 횟수 보고 (19 B). 모두 누적값 — 서버는 앞 보고와의 차이로 셈."""
    if len(buf) != COUNTS_LEN:
        raise ValueError(f"길이가 {len(buf)} 바이트임 ({COUNTS_LEN} 여야 함)")
    ver, seq, cooking, depl, vent, leaks = struct.unpack(COUNTS_FMT, buf)
    if ver != 1:
        raise ValueError(f"모르는 형식 버전 {ver}")
    return {"seq": seq, "cooking": cooking, "depletions": depl, "vent_minutes": vent, "leaks": leaks}


CAL_LEN = 34
CAL_NAMES = ["c_re_a", "c_re_b", "c_re_min", "throat_mm", "inlet_mm", "visc_upas", "visc_s_k", "molar_gmol"]


def decode_cal(buf: bytes) -> dict:
    """포트 4 교정값 보고 (34 B): 형식 1 · 넣은 칸 표시(비트 k = 칸 k+1) · 지금 쓰는 값 8개 float32 큰 쪽 먼저.
    펌웨어 Core/Inc/app.h APP_CFG_CAL · 설정 명령 05 를 받을 때마다 옴. 넣지 않은 칸은 펌웨어 기본값."""
    if len(buf) != CAL_LEN:
        raise ValueError(f"길이가 {len(buf)} 바이트임 ({CAL_LEN} 여야 함)")
    if buf[0] != 1:
        raise ValueError(f"모르는 형식 버전 {buf[0]}")
    vals = struct.unpack(">8f", buf[2:])
    d = dict(zip(CAL_NAMES, vals))
    d["set_fields"] = [n for k, n in enumerate(CAL_NAMES) if buf[1] & (1 << k)]
    d["has"] = buf[1]
    return d


def selftest(path: str) -> int:
    keys = ["seq", "unix_s", "total_nm3", "flow_nm3h", "pg_pa", "dp_pa", "t_c", "patm_pa", "batt_mv", "flags",
            "sdp810_pa", "ds18b20_c"]
    bad = n = 0
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        if line.startswith("K:"):
            hexs, has, *vals = line[2:].split("|")
            got = decode_cal(bytes.fromhex(hexs))
            n += 1
            if got["has"] != int(has):
                bad += 1
                print(f"  FAIL 교정값 넣은 칸: 파이썬 {got['has']} / C {has}")
            for k, v in zip(CAL_NAMES, map(float, vals)):
                n += 1
                if abs(got[k] - v) > 1e-6 * max(1.0, abs(v)):     # float32 로 보냄
                    bad += 1
                    print(f"  FAIL 교정값 {k}: 파이썬 {got[k]} / C {v}  ({hexs})")
            continue
        if line.startswith("C:"):
            hexs, *vals = line[2:].split("|")
            got = decode_counts(bytes.fromhex(hexs))
            for k, v in zip(["seq", "cooking", "depletions", "vent_minutes", "leaks"], vals):
                n += 1
                if got[k] != int(v):
                    bad += 1
                    print(f"  FAIL 횟수 {k}: 파이썬 {got[k]} / C 입력 {v}  ({hexs})")
            continue
        hexs, *vals = line.split("|")
        want = dict(zip(keys, map(float, vals)))
        got = decode(bytes.fromhex(hexs))
        for k in keys:
            n += 1
            if k == "flags":
                ok = got[k] == int(want[k])
            elif want[k] != want[k]:          # C 쪽 NAN = 없음
                ok = got[k] is None
            elif got[k] is None:
                ok = False
            else:
                ok = abs(got[k] - want[k]) <= TOL.get(k, 0) + 1e-9
            if not ok:
                bad += 1
                print(f"  FAIL {k}: 파이썬 {got[k]} / C 입력 {want[k]}  ({hexs})")
    print(f"  파이썬 풀기 대조: {n - bad} / {n} 통과")
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--selftest":
        sys.exit(selftest(sys.argv[2]))
    if len(sys.argv) == 3 and sys.argv[1] == "--cal":
        for k, v in decode_cal(bytes.fromhex(sys.argv[2])).items():
            print(f"{k:12s} {v}")
        sys.exit(0)
    if len(sys.argv) == 3 and sys.argv[1] == "--counts":
        for k, v in decode_counts(bytes.fromhex(sys.argv[2])).items():
            print(f"{k:12s} {v}")
        sys.exit(0)
    if len(sys.argv) == 2:
        for k, v in decode(bytes.fromhex(sys.argv[1])).items():
            print(f"{k:12s} {v}")
        sys.exit(0)
    print(__doc__)
    sys.exit(2)
