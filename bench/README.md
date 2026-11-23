# STM32 센서 벤치 (맥으로 옮김)

> **2026-11-23 맥으로 옮긴 상태.** 아래 원래 안내(윈도우 기준) 중 맥에서 바뀐 것만 먼저 적음.
>
> | 항목 | 맥에서 |
> |---|---|
> | 폴더 | 이 저장소 `bench/` — `firmware/`(Zephyr 앱) · `server/`(수집기) · `mosquitto/` · `claude-page/` · `sample_logs/` |
> | 센서 드라이버 | 저장소 `Core/` 의 hsc · abp2 를 그대로 씀(윈도우 묶음의 것과 줄바꿈만 달랐음) |
> | 비밀값 | `firmware/secrets.conf`, `server/credentials.txt` — git 에 안 올라감(.gitignore) |
> | 브로커 | Homebrew(인텔판)로는 안 깔려서 mosquitto 2.0.23 을 소스에서 arm64 로 빌드, `~/toolchains/mosquitto`. TLS 없이 빌드해서 `etc/passwd` 는 `사용자:비밀번호` 평문(chmod 600). 설정 `~/toolchains/mosquitto/etc/mosquitto.conf`, 기록 `~/toolchains/mosquitto/log/mosquitto.log` |
> | 브로커 켜기 | `nohup ~/toolchains/mosquitto/sbin/mosquitto -c ~/toolchains/mosquitto/etc/mosquitto.conf >/dev/null 2>&1 &` |
> | 수집기 | 상시 실행은 아래 launchd. 손으로 시험할 땐 `cd server && source .venv/bin/activate && python collector.py` (맥에서 시험 58개 통과) |
> | 맥 주소 | `prj.conf` 의 `CONFIG_BENCH_MQTT_BROKER="172.30.1.93"` (맥 Wi-Fi en0). 공유기에서 이 주소를 맥에 고정해야 함 |
> | 빌드 | `~/zephyrproject`(Zephyr f545c1ca45b, 윈도우와 같은 커밋 · 모듈은 cmsis · cmsis_6 · hal_stm32 만) + `~/toolchains/arm-gnu-14.2.rel1` |
>
> ```
> cd ~/zephyrproject && source .venv/bin/activate
> export ZEPHYR_BASE=$HOME/zephyrproject/zephyr ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb GNUARMEMB_TOOLCHAIN_PATH=$HOME/toolchains/arm-gnu-14.2.rel1
> west build -b nucleo_h563zi -d <저장소>/bench/firmware/build_bench -p always <저장소>/bench/firmware
> ```
>
> 굽기(맥, 11/23 성공 · 4초): STM32CubeProgrammer 대신 probe-rs 로 굽고 리셋함. 구운 뒤 보드가 DHCP 로 주소를 받고 맥 브로커에 붙어 수집기 CSV 가 1초에 10줄씩 쌓이는 것까지 확인.
>
> ```
> probe-rs download --chip STM32H563ZITx --verify <저장소>/bench/firmware/build_bench/zephyr/zephyr.elf
> probe-rs reset --chip STM32H563ZITx
> ```
>
> 옮기기 전 윈도우 판 펌웨어 앞 256 KB 백업: `~/toolchains/backup/nucleo_h563zi_windows_bench_20261123_256k.txt` (probe-rs read 결과, 주소 · 32비트 워드 글자).
>
> 상시 실행(11/23): 로그인 때 launchd 가 둘 다 띄우고, 죽으면 5초 뒤 다시 켬.
> - 브로커 `~/Library/LaunchAgents/com.samton.bench.mosquitto.plist`
> - 수집기 `~/Library/LaunchAgents/com.samton.bench.collector.plist` → **실행본은 `~/bench/server`**, 기록도 `~/bench/server/logs/`. launchd 로 띄운 프로그램은 macOS 보안 때문에 `~/Documents` 를 못 읽어서 저장소에서 돌리지 못함.
> - 저장소의 `server/` 를 고친 뒤엔 `sh server/deploy_mac.sh` 한 번 → 복사하고 수집기를 다시 켬.
> - 상태 `launchctl print gui/$(id -u)/com.samton.bench.collector`, 멈춤 `launchctl bootout gui/$(id -u)/com.samton.bench.collector`, 다시 등록 `launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.samton.bench.collector.plist` (브로커도 같은 식).
>
> 실시간 페이지(아티팩트 https://claude.ai/artifact/YN7VeeQhmvBifBkXv9NecE): 페이지는 이 맥의 로컬 MCP 서버 `nucleo-h563zi-breadboard`(`claude-page/breadboard_server.py`)에서 값을 받음. 11/23 Claude Code 사용자 범위로 등록 — 파이썬 `~/bench/mcp/.venv`(mcp 1.x, `claude-page/requirements.txt`), 환경변수 BENCH_PORT=/dev/cu.usbmodem21503 · BENCH_LOG_DIR=~/bench/server/logs · BENCH_MQTT_CRED=~/bench/server/credentials.txt · BENCH_VENTURI_DIR=~/bench/server. 확인 `claude mcp get nucleo-h563zi-breadboard`. mcp 2.x 는 FastMCP 이름이 바뀌어 안 뜸.

---

# (원래 안내) STM32 센서 서버 옮기기 (윈도우 → 맥)

NUCLEO-H563ZI 보드가 압력 센서 3개(HSC · ABP2 · SDP810)를 0.1초마다, 대기압 센서(BME280)와 가스 온도 센서(DS18B20)를 1초마다 읽어 랜선 MQTT 로 보냄. 수집기는 받은 줄을 모아 1초마다 한꺼번에(10줄씩) 파일에 씀.
이 폴더는 그 값을 받는 서버를 다른 PC(맥)로 옮기는 데 필요한 것을 모은 것임.

자세한 설명(센서, 통신 규격, 로그 형식)은 인수인계 문서에 있음:
https://claude.ai/artifact/ShFKYDtXbs58NjoWmxHPkA

배선은 여기:
https://claude.ai/artifact/1Ztv8tLEYPaSRbK39Utyh4

## 폴더 구성

| 폴더 | 내용 |
|---|---|
| `server/` | **수집기** `collector.py`. MQTT 로 받아 CSV 로 남김. 맥에서 돌릴 것은 이것 하나 |
| `mosquitto/` | 맥용 MQTT 브로커 설정 예시 |
| `secrets/` | 지금 쓰는 MQTT 계정 (비밀번호 들어 있음. **남에게 넘길 땐 이 폴더 빼기**) |
| `firmware/` | 보드 펌웨어 소스 (Zephyr). 서버 주소를 바꾸려면 다시 구워야 해서 넣음. BME280(I2C4 0x76) · DS18B20(D2 = PG14, 1-Wire) 읽기 포함 |
| `claude-page/` | Claude 데스크톱 앱에서 보는 실시간 페이지와 그 MCP 서버 (선택) |
| `sample_logs/` | 실제로 쌓인 로그 예시 |

## 맥에서 할 일

1. **맥 주소 고정.** 공유기 설정에서 맥에 항상 같은 주소가 가게 함(예: 172.30.1.10). 보드와 같은 공유기여야 함.

2. **브로커 설치.**
   ```
   brew install mosquitto
   cp mosquitto/mosquitto.conf /opt/homebrew/etc/mosquitto/mosquitto.conf
   mosquitto_passwd -c /opt/homebrew/etc/mosquitto/passwd h563     # 비밀번호는 secrets/credentials.txt 의 것
   brew services start mosquitto
   ```
   맥 방화벽이 켜져 있으면 mosquitto 의 들어오는 연결을 허용해야 함.

3. **수집기 실행.**
   ```
   cd server
   python3 -m venv .venv && source .venv/bin/activate
   pip install -r requirements.txt
   cp ../secrets/credentials.txt credentials.txt
   python collector.py
   ```
   `브로커 127.0.0.1:1883 연결됨` 이 나오면 됨. 기록은 `server/logs/` 에 쌓임.
   다른 폴더에 쌓으려면 `BENCH_LOG_DIR=/원하는/경로 python collector.py`.

4. **보드가 맥을 보게 바꾸기 (윈도우 PC 에서).** 보드 펌웨어에 서버 주소가 박혀 있어서 다시 구워야 함.
   - `firmware/bench/prj.conf` 맨 아래 `CONFIG_BENCH_MQTT_BROKER="172.30.1.1"` 을 맥 주소로 바꿈.
   - `secrets/secrets.conf` 를 `firmware/bench/secrets.conf` 로 복사.
   - 보드 USB 를 윈도우 PC 에 꽂고 빌드 · 굽기 (Git Bash, `C:\Users\solst\zephyrproject`):
     ```
     source .venv/Scripts/activate
     export ZEPHYR_BASE=/c/Users/solst/zephyrproject/zephyr
     west build -b nucleo_h563zi -d build_bench -p always <이 폴더>/firmware/bench
     west flash -d build_bench --runner stm32cubeprogrammer --conn-modifiers=mode=UR
     ```
   - 맥에서는 굽지 말 것. ST-LINK 가 혼자 끊겨서 실패했음.

5. **확인.** 수집기를 켜 둔 채 보드를 켜면 `server/logs/오늘날짜/시.csv` 에 1초마다 10줄씩 늘어남.
   안 늘면 `server/logs/board.log` 와 수집기 화면을 봄.

## 윈도우에서 수집기 켜 두기 (작업 스케줄러)

수집기를 Claude 앱 안에서 띄우면 앱이 꺼지거나 다시 켜질 때 같이 죽음. 그래서 윈도우 PC 에서는 작업 스케줄러가 띄움.

- 등록(한 번만, 관리자 권한 필요 없음): `server/register_collector_task.ps1` 실행 → 작업 이름 `STM bench collector`, 로그온 때 시작.
- 작업은 `server/run_collector.ps1` 을 돌리고, 이 스크립트가 수집기를 켜 둠. 수집기가 죽으면 5초 뒤 다시 켬.
- 로그는 덮어쓰지 않고 덧붙임: `collector.out.log`, `collector.err.log`(죽은 이유), `collector.run.log`(켜고 끈 시각 · 종료 코드). 모두 기록 폴더에 있음.
- 상태 `Get-ScheduledTask 'STM bench collector'`, 멈춤 `server/stop_collector.ps1`, 다시 켬 `Start-ScheduledTask …`, 지움 `Unregister-ScheduledTask … -Confirm:$false`.
- `Stop-ScheduledTask` 만 하면 스크립트만 꺼지고 파이썬 수집기는 남음. 그래서 끌 때는 `stop_collector.ps1` 을 씀. (남아 있어도 다음에 켤 때 `run_collector.ps1` 이 먼저 끄고 시작함)
- 경로(기록 폴더 · 파이썬 · 계정 파일)는 `run_collector.ps1` 맨 위에서 바꿈. 맥에서는 이 두 파일 대신 launchd 나 `nohup` 으로 띄우면 됨.

## 랜선이 빠졌을 때 (USB 시리얼로 대신 받기)

보드는 같은 줄을 랜선(MQTT)과 USB 시리얼 양쪽으로 찍음. 랜선으로 줄이 5초 넘게 안 오면 수집기가 보드 USB 시리얼에서 읽어 그대로 기록함.

- 켜는 법: 환경변수 `BENCH_SERIAL_PORT` 에 포트 이름 (윈도우 `COM3`, 맥 `/dev/tty.usbmodem…`). 비워 두면 안 씀. `run_collector.ps1` 에는 `COM3` 로 들어 있음. `pip install pyserial` 필요.
- 보드 USB 가 수집기가 도는 PC 에 꽂혀 있어야 함. 서버를 맥으로 옮긴 뒤에는 보드 USB 도 맥에 꽂아야 이 기능이 됨.
- CSV 의 `src` 열로 구분: `mqtt` = 랜선, `serial` = USB. 유량 · 적산 계산은 똑같음.
- 랜선으로 30초 동안 다시 잘 오면 포트를 놓고 랜선 줄만 씀. 두 길로 같은 줄이 오면 하나만 남김(줄 번호로 가림).
- COM 포트는 한 프로그램만 열 수 있음. 그래서 포트를 잡은 쪽이 읽은 측정 줄을 브로커의 `h563/bench/usb` 토픽으로 다시 보내고, 못 잡은 쪽이 그걸 받음. 수집기가 잡으면 Claude 페이지 서버가 받고, 페이지 서버가 잡으면 수집기가 받아 적음.
- 시리얼 터미널(보드 셸)을 써야 하면 기록 폴더에 빈 파일 `serial.pause` 를 만듦. 있는 동안은 수집기도 페이지 서버도 포트를 안 잡음(그동안 랜선이 없으면 기록도 멈춤). 끝나면 파일을 지움. 5초쯤 뒤 다시 받음.
- 포트를 잡고 놓은 때는 `board.log` 에 `[수집기] …` 로 남음. 랜선이 없을 때 보드가 10초마다 찍는 "주소 없음" 안내는 10분에 한 번만 남김.
- 줄이 끊긴 뒤 시리얼로 넘어가기까지 약 5초는 기록이 빔.

## 로그

```
<기록 폴더>/
  2026-11-18/
    12.csv      12시대 측정. 첫 줄이 열 이름
    12.jsonl    (기본은 안 남김. BENCH_JSONL=1 로 켜면) 한 줄에 JSON 하나씩, 보드 원문 raw 포함
  board.log     측정 줄이 아닌 보드 안내 (영점, I2C 스캔, 접속 · 끊김)
  latest.json   가장 최근 측정 하나 (웹 페이지가 읽기 쉬우라고 매번 덮어씀)
  totals.json   적산(누적 유량). 수집기를 껐다 켜도 이어지게 1초마다 저장
```

CSV 열(30개): `time, seq, src, period_ms, read_us, hsc_state, hsc_pa, hsc_c, abp2_state, abp2_pa, abp2_c, abp2_status, sdp_state, sdp_pa, sdp_c, bme_state, bme_pa, bme_c, bme_rh, ds_state, ds_c, rho_kgm3, flow_sdp_m3h, flow_sdp_nm3h, flow_hsc_m3h, flow_hsc_nm3h, total_hsc_m3, total_hsc_nm3, total_sdp_m3, total_sdp_nm3`

- 끝의 9 열(밀도 · 유량 4개 · 적산 4개)은 수집기가 계산해 붙이는 값. 아래 "유량", "적산" 절 참고.
- `bme_*` 는 BME280(대기압 Pa · 센서 둘레 온도 °C · 습도 %), `ds_c` 는 DS18B20(관 속 가스 온도 °C). 보드가 1초마다 읽고 그 값을 0.1초 줄마다 다시 붙임. 2026-11-20 21:51 부터 있는 열이고, 그 전 파일(24열)은 `시_old….csv` 로 옆에 남아 있음.

- `time` 은 수집기가 받은 시각(그 PC 의 현지 시각). 보드에는 시계가 없음.
- `seq` 는 보드가 켜질 때마다 1부터 다시 시작함.
- 상태가 `정상` 이 아니면 압력 · 온도 칸이 비어 있을 수 있음.
- 측정 주기는 0.1초(`CONFIG_BENCH_SAMPLE_PERIOD_MS=100`)라 1초에 10줄. 파일은 1초마다 한 번 열어 10줄을 한꺼번에 씀(`BENCH_FLUSH_S`).
- 기록은 CSV 만 남김. 하루 약 86만 줄, 약 170 MB(한 줄 약 195 바이트). 시간별 JSON 은 기본으로 꺼져 있고(`BENCH_JSONL=1` 로 켬, 하루 약 400 MB), 웹 페이지용 `latest.json` 한 파일은 항상 남음.

## 유량 (벤투리)

- 수집기가 줄마다 벤투리 유량을 계산해 CSV 끝 열과 `latest.json` 의 `flow` 에 넣음: `rho_kgm3`, `flow_sdp_m3h`, `flow_sdp_nm3h`, `flow_hsc_m3h`, `flow_hsc_nm3h`.
- 식은 문서 "바이오가스 계량기 계산식" 2 · 3절의 식 (`server/venturi.py`). 밀도는 일반 공기 기준(28.965 g/mol, 건조 공기라 습도는 안 씀). 절대압 = BME280 대기압 + ABP2 정압, 온도 = DS18B20 가스 온도 (2026-11-20 22:01 부터. 그 전에는 대기압 101,325 Pa 고정 · SDP810 칩 온도). BME280 이 안 읽히면 `venturi.json` 의 `p_atm_pa`, DS18B20 이 안 읽히면 SDP810 → HSC → ABP2 칩 온도로 대신함. 실제로 쓴 값과 출처는 `latest.json` 의 `flow.p_atm` · `flow.t`.
- 치수 · 가스 · 대기압은 `server/venturi.json`. 배치도 · 구성 상세 문서(https://claude.ai/artifact/FY1zFy8jA7vNj9TMuxcZ7G) 과 실물 확인으로 입구 16 mm, 목 4 mm(β 0.25, 문서에는 3.5 mm 로 적혀 있지만 실물은 4 mm), 가스는 일반 공기(0.028965 kg/mol). 현장 바이오가스(메탄 60 %, CO₂ 40 %)면 `gas_molar_kg` 를 0.02723 으로. 유출계수 C 는 11/20 가스계량기 비교 시험으로 맞춘 센서별 C(Re) 식(`venturi.json` 의 `C_re`, 시험 범위 Re 약 1,700~5,900 밖은 끝 값으로 고정). 이 계수는 시험 때 조건을 대기압 102,040 Pa · 가스 온도 25.0 °C 로 보고 새 밀도 기준으로 옮긴 값이라(C 약 0.57 % 커짐), 새 센서를 단 채로 계량기 시험을 다시 하면 그 값으로 바꿀 것. `p_atm_pa`(101,325)는 BME280 이 없을 때만 쓰는 대체값. 바꾸면 수집기를 다시 켤 것.
- 저유량 컷오프: 차압 0.5 Pa 미만은 유량 0 (`dp_cutoff_pa`). 0 점 잡음이 적산에 헛으로 쌓이지 않게.

## 교정 (유출계수 다시 맞추기)

유량 오차의 대부분은 유출계수 C 를 맞춘 시험 자료의 품질에서 옴(점이 적고, 한 점이 30~80 L 로 짧음). 그래서 계량기 비교 시험을 쌓아 가며 식을 다시 맞추는 길을 만들어 둠.

- 지금 쓰는 식은 `server/venturi_cal.json` (있으면 `venturi.json` 의 `C_re` 를 센서별로 덮어씀). 2026-11-20 22:20 부터 **C = a + b ÷ Re** (`"model": "inv"`): HSC a 0.97739 · b −158.91 (Re 1,725 아래는 끝 값), SDP810 a 0.93671 · b −134.55 (Re 1,774 아래는 끝 값). 낮 시험 7점 중 6점으로 맞춤(0.5 m³/h 점은 두 센서가 똑같이 −2.5 % 벗어나 계량기 읽기 쪽 문제로 보고 뺌). 남은 6점의 오차는 ±0.3 % 안쪽. 그 전의 log 식(`venturi.json` 에 그대로 있음)은 같은 점에서 ±1.5 %.
- 실시간 페이지 맨 아래쪽 **교정** 표가 이 일을 함: 구간 적산 타이머로 잰 뒤 '계량기 실제'를 넣고 **교정에 넣기** → 점이 표에 쌓임 → 표가 새 식과 점별 오차를 보여 줌 → **새 식 적용**.
  - 점마다 '기준 한계'(계량기 눈금 읽기 ±0.1 L 두 번 + 시작 · 끝 맞춤 ±1초씩)를 계산해, 한계가 작은 점(오래 잰 점)을 무겁게 침. 유량점마다 100 L 넘게 흘리면 한계가 ±0.5 % 아래.
  - 새 식으로도 기준 한계의 2배(최소 1.5 %)를 넘게 벗어나는 점은 색으로 표시됨. '쓰기' 체크를 끄면 그 점을 빼고 다시 맞춤.
  - 점 목록은 페이지의 공유 저장소(`live/cal`)에 있어 어느 기기에서나 보임. 적용은 보드 PC 페이지가 MCP 서버의 `set_c_re` 툴로 `venturi_cal.json` 을 쓰고(이전 값은 파일의 `history` 에 20개까지 남음), 수집기와 MCP 서버가 1초 안에 알아서 다시 읽음. 껐다 켤 필요 없음. `board.log` 에 `[설정] 벤투리 설정 다시 읽음` 줄이 남음.
  - `set_c_re` 는 처음 한 번 보드 PC 의 Claude 앱에서 허용을 눌러 줘야 함.
- 적용한 뒤의 줄만 새 식으로 계산됨. 그 전 CSV 의 유량 · 적산은 그때 식 그대로.
- 되돌리려면 `venturi_cal.json` 의 `history` 값을 다시 적용하거나, 파일을 지우면 `venturi.json` 의 식으로 돌아감.
- 맥에서 페이지 없이 수집기만 돌릴 때는 `venturi_cal.json` 을 손으로 고치면 됨(같은 형식). C 가 0.5~1.05 를 벗어나는 값은 무시됨.

## 적산 (누적 유량)

- 수집기가 줄마다 유량 × 측정 주기(0.1초)를 더함. CSV 끝 4 열 `total_hsc_m3`, `total_hsc_nm3`, `total_sdp_m3`, `total_sdp_nm3` 가 누적 부피(m³). `latest.json` 의 `totals` 에 오늘 누적(`today`)과 시작 시각(`since`)도 있음.
- `<기록 폴더>/totals.json` 에 1초마다 저장되어 껐다 켜도 이어짐. 처음부터 세려면 수집기를 끄고 `totals.json` 을 지움.
- 센서 오류 줄과 보드가 꺼져 있던 시간은 안 더함.
- HSC 는 눈금이 0.15 Pa 로 거칠어서 펌웨어가 세 가지를 함: ① 한 번 측정에 16번 읽어 평균 ② 1초 이동 평균(0.6 Pa 넘는 변화는 바로 따라감) ③ 흐름이 없을 때(SDP810 ±1 Pa 안쪽) 0점을 SDP810 에 천천히 맞춤. 흔들림(표준편차)이 0.24 → 0.03 Pa 로 줄었음. 설정은 `CONFIG_BENCH_HSC_OVERSAMPLE`, `_SMOOTH_TAU_MS`, `_SMOOTH_JUMP_MPA`, `_TRACK_SDP`, `_TRACK_TAU_S`.
- 그래도 0 근처에서는 HSC 유량이 0 ~ 수십 L/h 로 보일 수 있음(작은 흔들림이 제곱근으로 커짐). 낮은 유량은 SDP810 쪽이 기준.

## HSC 보정 (SDP810 기준)

- 펌웨어: `CONFIG_BENCH_HSC_GAIN_PPM`, `CONFIG_BENCH_HSC_OFFSET_MPA` (기본 1.0, 0). 찍는 값 = 기울기 × (읽은 값 − 켜질 때 영점) + 오프셋.
- 계산: 두 센서를 T자 관으로 같은 압력에 물리고 몇 단계 압력에서 기록한 뒤 `python server/hsc_fit.py <csv> --from HH:MM:SS --to HH:MM:SS`. 나온 두 줄을 `firmware/bench/prj.conf` 에 넣고 다시 굽기.
- 이미 보정이 들어간 보드의 로그로 다시 맞출 땐 `--gain-ppm`, `--offset-mpa` 에 그때 값을 넣어야 겹쳐서 계산됨 (부팅 때 board.log 에 찍힘).

## AWS 서버에 올릴 때

- 수집기를 **맥**에서 돌리면 CSV 는 맥의 `logs/` 에 쌓임. AWS 에서 보려면 주기적으로 올려야 함(rsync, S3 등).
- 수집기를 **AWS** 에서 돌리면 CSV 는 AWS 서버의 `logs/` 에 쌓임. 웹 라우트로 파일 목록 · 다운로드를 열거나 SSH 로 내려받아 봄.
- 보드는 암호화 없는 MQTT(1883)로만 보내고 숫자 IP 만 받음. 보드가 AWS 로 직접 보내면 비밀번호와 값이 인터넷에 그대로 노출됨. 권장: 보드 → 맥 브로커 → (TLS) → AWS. 맥 Mosquitto 에 브리지를 걸거나, 맥에서 수집한 CSV 를 올림.
- AWS 서버는 보통 UTC 라 `time` 열과 날짜 폴더가 9시간 늦게 찍힘. `TZ=Asia/Seoul python collector.py` 로 돌리거나 알고 쓸 것.
- 인터넷에 내놓기 전에 MQTT 비밀번호를 새로 바꿀 것. CSV 를 웹으로 열 때는 로그인 뒤에만 보이게 할 것.

## 웹 페이지에 올릴 때

- 가장 쉬운 길: 웹 서버가 `latest.json`(최신 값)과 `날짜/시.csv`(기록)를 읽어서 보여 줌.
- 실시간으로 받고 싶으면 웹 서버가 MQTT 토픽 `h563/bench/line` 을 직접 구독해도 됨. 줄 문법과 정규식은 `collector.py` 의 `parse_line` 그대로 쓰면 됨.
- 화면 디자인은 `claude-page/breadboard.html` 을 기준으로 삼으면 됨. 샘튼 브랜드 가이드(네이비 #081521 · #0c1f3b · #133254, 흰 글자 #fafcfe, 글꼴 Pretendard GOV)를 따른 화면: 왼쪽 채널 목록(ABP2 · HSC · SDP810 · 유량 둘), 오른쪽 큰 그래프(현재값 · 구간 통계 · 마우스 십자선 · 구간 선택 막대), 아래 적산 · 가스 밀도 요약. 밝은 테마 · 폰 화면도 됨.
- 이 화면의 원본은 `claude-page/web/breadboard.tpl.html`, 만드는 스크립트는 `web/build_page.py`(글꼴을 쓰인 글자만 남겨 넣고 샘튼 로고 벡터를 넣음). 글꼴 파일(5.4 MB)은 묶음에 없어서 스크립트가 알려 주는 주소에서 받아 `web/fonts/` 에 두면 됨.

## 아직 확인 안 된 것

- `collector.py` 는 윈도우에서만 돌려 봤음. 맥에서는 아직 안 돌려 봄(잠금 부분만 맥용 코드가 따로 있음).
- 맥용 Mosquitto 설정 경로는 Apple Silicon Homebrew 기준임. 인텔 맥은 `/usr/local/etc/mosquitto/`.
