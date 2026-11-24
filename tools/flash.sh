#!/bin/zsh
# STLINK 로 펌웨어 넣기: ./tools/flash.sh [elf 경로]  (기본 = PCB3_2 판)
set -euo pipefail
source ~/toolchains/env.sh
ELF="${1:-build.nosync/gasmeter_pcb3_2.elf}"   # 다음 판: build.nosync/gasmeter_pcb3_3.elf
[[ -f "$ELF" ]] || { echo "빌드 결과가 없음: $ELF (먼저 cmake --build build.nosync)"; exit 1; }
"$STM32_PRG_CLI" -c port=SWD mode=UR reset=HWrst -w "$ELF" -v -rst
