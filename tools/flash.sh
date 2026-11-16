#!/bin/zsh
# STLINK 로 펌웨어 넣기: ./tools/flash.sh [elf 경로]
set -euo pipefail
source ~/toolchains/env.sh
ELF="${1:-build.nosync/gasmeter.elf}"
[[ -f "$ELF" ]] || { echo "빌드 결과가 없음: $ELF (먼저 cmake --build build.nosync)"; exit 1; }
"$STM32_PRG_CLI" -c port=SWD mode=UR reset=HWrst -w "$ELF" -v -rst
