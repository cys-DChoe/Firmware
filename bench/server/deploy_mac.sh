#!/bin/sh
# 맥에서 수집기를 launchd 로 상시 실행하기 위한 배포.
# launchd 로 띄운 프로그램은 macOS 보안(TCC) 때문에 ~/Documents 를 못 읽음 → 실행본을 ~/bench/server 로 복사해서 돌림.
# 이 폴더(저장소)의 코드 · 설정을 고친 뒤 이 스크립트를 터미널에서 한 번 돌리면 복사하고 수집기를 다시 켬.
# 기록(logs/)과 가상환경(.venv)은 ~/bench/server 쪽 것을 그대로 둠.
# 교정값 venturi_cal.json 은 실시간 페이지가 ~/bench/server 쪽에 직접 씀 → 덮어쓰지 않고, 없을 때만 복사함.
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
DST="$HOME/bench/server"
mkdir -p "$DST"
rsync -a --exclude .venv --exclude logs --exclude __pycache__ --exclude .pytest_cache --exclude venturi_cal.json "$SRC/" "$DST/"
[ -f "$DST/venturi_cal.json" ] || cp "$SRC/venturi_cal.json" "$DST/venturi_cal.json"
chmod 600 "$DST/credentials.txt" 2>/dev/null || true
if [ ! -x "$DST/.venv/bin/python" ]; then
  /usr/local/bin/python3 -m venv "$DST/.venv"
  "$DST/.venv/bin/pip" install -q -r "$DST/requirements.txt"
fi
launchctl kickstart -k "gui/$(id -u)/com.samton.bench.collector" 2>/dev/null && echo "수집기 다시 켬" || echo "launchd 작업(com.samton.bench.collector)이 아직 없음"
echo "배포: $DST"
