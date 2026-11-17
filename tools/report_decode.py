#!/usr/bin/env python3
"""1분 보고(형식 v1, 24바이트) 풀기 — 서버 쪽 참고 구현.

펌웨어의 Core/Inc/report.h 표와 같은 형식. C 코드와 따로 짰고,
tests/host/run.sh 가 C 가 묶은 값을 이걸로 풀어 맞는지 확인함.

쓰는 법:
    python3 tools/report_decode.py 0101020...      # 16진 문자열 하나 풀기
    python3 tools/report_decode.py --selftest FILE # 시험 벡터 대조
"""
import struct
import sys

LEN = 24
FMT = ">BHIIHhhhHBH"   # 버전 · 순번 · 시각 · 누적 · 유량 · 정압 · 차압 · 온도 · 대기압 · 배터리 · 상태

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
]


def decode(buf: bytes) -> dict:
    if len(buf) != LEN:
        raise ValueError(f"길이가 {len(buf)} 바이트임 (24 여야 함)")
    ver, seq, unix_s, total, flow, pg, dp, t, patm, batt, flags = struct.unpack(FMT, buf)
    if ver != 1:
        raise ValueError(f"모르는 형식 버전 {ver}")
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
    }


# 눈금의 절반 = 반올림으로 생길 수 있는 최대 차이
TOL = {"total_nm3": 0.00005, "flow_nm3h": 0.0005, "pg_pa": 0.5, "dp_pa": 0.025,
       "t_c": 0.05, "patm_pa": 0.5, "batt_mv": 5}


def selftest(path: str) -> int:
    keys = ["seq", "unix_s", "total_nm3", "flow_nm3h", "pg_pa", "dp_pa", "t_c", "patm_pa", "batt_mv", "flags"]
    bad = n = 0
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        hexs, *vals = line.split("|")
        want = dict(zip(keys, map(float, vals)))
        got = decode(bytes.fromhex(hexs))
        for k in keys:
            n += 1
            if k == "flags":
                ok = got[k] == int(want[k])
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
    if len(sys.argv) == 2:
        for k, v in decode(bytes.fromhex(sys.argv[1])).items():
            print(f"{k:12s} {v}")
        sys.exit(0)
    print(__doc__)
    sys.exit(2)
