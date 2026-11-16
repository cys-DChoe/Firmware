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
