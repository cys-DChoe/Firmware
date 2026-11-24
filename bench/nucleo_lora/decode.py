#!/usr/bin/env python3
"""Nucleo LoRa 벤치 보고 풀기 — 11/24 부터 보드와 같은 형식 v2(28 B) 라 tools/report_decode.py 를 그대로 씀.

    python3 bench/nucleo_lora/decode.py 0200…   # ChirpStack 이벤트의 Data 16진 문자열

11/23 벤치 꼬리(형식 v1 24 B + 4 B, 첫 바이트 01)도 풂: 뒤 4 B 를 v2 의 24 · 26 번 칸으로 봄.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import report_decode  # noqa: E402

NONE = -32768


def decode(buf: bytes) -> dict:
    if len(buf) == 28 and buf[0] == 1:          # 11/23 벤치 꼬리
        d = report_decode.decode(buf[:24])
        sdp, ds = struct.unpack(">hh", buf[24:])
        d["sdp810_pa"] = None if sdp == NONE else sdp / 50.0
        d["ds18b20_c"] = None if ds == NONE else ds / 100.0
        return d
    return report_decode.decode(buf)


if __name__ == "__main__":
    for k, v in decode(bytes.fromhex(sys.argv[1])).items():
        print(f"{k:12} {v}")
