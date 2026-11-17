#!/bin/zsh
# 맥에서 순수 계산 모듈 시험: ./tests/host/run.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=build.nosync/host
mkdir -p "$OUT"
CFLAGS=(-std=c11 -O1 -Wall -Wextra -Werror -ICore/Inc)

echo "== 계산 모듈 (meter)"
cc $CFLAGS -o "$OUT/test_meter" Core/Src/meter.c tests/host/test_meter.c -lm
"$OUT/test_meter"

echo "\n== 1분 보고 묶기 (report)"
cc $CFLAGS -o "$OUT/test_report" Core/Src/report.c tests/host/test_report.c -lm
"$OUT/test_report" "$OUT/report_vectors.txt"
python3 tools/report_decode.py --selftest "$OUT/report_vectors.txt"

echo "\n== 센서 드라이버 (HSC · ABP2 · BMP390)"
cc $CFLAGS -o "$OUT/test_sensors" Core/Src/hsc.c Core/Src/abp2.c Core/Src/bmp390.c Core/Src/sensors.c \
   Core/Src/i2c_timing.c tests/host/test_sensors.c -lm
"$OUT/test_sensors"

echo "\n== LoRa 모듈 AT (RAK3172 · RUI3)"
cc $CFLAGS -o "$OUT/test_rak3172" Core/Src/rak3172.c Core/Src/lowpower.c tests/host/fake_rak.c tests/host/test_rak3172.c
"$OUT/test_rak3172"

echo "\n== 외부 플래시 · 1분 기록 링 (W25Q128)"
cc $CFLAGS -o "$OUT/test_ring" Core/Src/w25q.c Core/Src/ring.c tests/host/fake_w25q.c tests/host/test_ring.c
"$OUT/test_ring"

echo "\n== 잠 모드 계산 (깨우기 타이머 · UART 속도 · 달력)"
cc $CFLAGS -o "$OUT/test_lowpower" Core/Src/lowpower.c tests/host/test_lowpower.c
"$OUT/test_lowpower"

echo "\n== 누적 · 설정 · 키 저장 (nvlog · persist)"
cc $CFLAGS -o "$OUT/test_nvlog" Core/Src/w25q.c Core/Src/ring.c Core/Src/nvlog.c Core/Src/persist.c Core/Src/nv_w25q.c \
   tests/host/fake_w25q.c tests/host/test_nvlog.c -lm
"$OUT/test_nvlog"

echo "\n== LTE 켜기 · 망 확인 · 끄기 (EG915U)"
cc $CFLAGS -Itests/host -o "$OUT/test_lte" Core/Src/lte.c tests/host/fake_lte.c tests/host/test_lte.c
"$OUT/test_lte"

echo "\n== 전체 흐름 (센서 → 계산 → 저장 → LoRa)"
cc $CFLAGS -Itests/host -o "$OUT/test_app" Core/Src/app.c Core/Src/meter.c Core/Src/report.c Core/Src/ring.c Core/Src/w25q.c \
   Core/Src/nvlog.c Core/Src/persist.c Core/Src/nv_w25q.c Core/Src/rak3172.c Core/Src/lowpower.c Core/Src/lte.c \
   tests/host/fake_w25q.c tests/host/fake_rak.c tests/host/fake_lte.c tests/host/test_app.c -lm
"$OUT/test_app"

echo "\n== 셀 전압 계산"
cc $CFLAGS -o "$OUT/test_batt" Core/Src/batt.c tests/host/test_batt.c
"$OUT/test_batt"
