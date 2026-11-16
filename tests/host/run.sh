#!/bin/zsh
# 계산 모듈을 맥에서 빌드해 시험: ./tests/host/run.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=build.nosync/host
mkdir -p "$OUT"
cc -std=c11 -O1 -Wall -Wextra -Werror -ICore/Inc -o "$OUT/test_meter" Core/Src/meter.c tests/host/test_meter.c -lm
"$OUT/test_meter"
