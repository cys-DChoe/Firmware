#!/usr/bin/env python3
"""Nucleo LoRa 벤치 보고(28바이트) 풀기 = 형식 v1 24바이트 + 벤치 꼬리 4바이트.

    python3 bench/nucleo_lora/decode.py 0100…   # ChirpStack 이벤트의 Data 16진 문자열

꼬리: SDP810 차압 60초 평균(0.02 Pa) · DS18B20 온도(0.01 °C), 0x8000 = 값 없음. 앞 24바이트는 tools/report_decode.py 로 풂.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import report_decode  # noqa: E402

NONE = -32768


def decode(buf: bytes) -> dict:
    if len(buf) != 28:
        raise ValueError(f"길이가 {len(buf)} 바이트임 (28 이어야 함)")
    d = report_decode.decode(buf[:24])
    sdp, ds = struct.unpack(">hh", buf[24:])
    d["sdp810_pa"] = None if sdp == NONE else sdp / 50.0
    d["ds18b20_c"] = None if ds == NONE else ds / 100.0
    return d


if __name__ == "__main__":
    for k, v in decode(bytes.fromhex(sys.argv[1])).items():
        print(f"{k:12} {v}")
