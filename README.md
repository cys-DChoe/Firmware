# Firmware

가스 계량기(GasMeter_19) 펌웨어. MCU 는 STM32H563ZGT6. 판은 PCB3_2(11/18 주문) · PCB3_3(다음 판) 두 가지 — 공통 코드는 `Core/`, 판마다 다른 것만 `boards/<판>/`.

- 설계서(하는 일 · 정한 것 9가지 · 보드 오면 하는 순서): https://claude.ai/artifact/4pURFGYAHrpZGocGMBt1hs
- 핀 · 판 차이: `boards/pcb3_2/` · `boards/pcb3_3/` (아래 "판별 폴더")

## 지금 들어 있는 것 (0.1.0-firstlight)

보드가 오면 처음 넣어 볼 프로그램.

- LED1 이 0.5초마다 깜빡임
- 로그(USART3, J_DBG 헤더, 115200)에 1초마다 시간 · 버튼 상태를 찍음
- 32.768 kHz 시계(LSE)를 켜 보고 결과를 로그에 남김. 실패하면 LED2 켜짐
- 켜지자마자 센서 전원 끔, LTE PWRKEY · RST 는 누르지 않음

센서 · 플래시 · LoRa · LTE · 잠 모드는 다음 단계.

## 계산 모듈 (`Core/Src/meter.c`)

하드웨어와 무관한 순수 계산. 계산식 문서(https://claude.ai/artifact/MAQopiVVYw6BqANQ25Xx32)의 2–8절 중 계량기에서 하는 것만 담음: 유량 · 표준상태 환산 · 사용량 적산 · 영점 · 조리 이벤트 · 고갈 · 방출 시간 · 계량기 뒤 누설. 방출량 · 계량기 앞 누설 · 이용률은 서버 몫(설계서 3번).

맥에서 시험 (문서 예제 1–6 과 대조):

```bash
./tests/host/run.sh
```

판정 문턱은 문서의 제안값이라 `meter_default_cfg()` 에 설정값으로 모여 있음.

벤투리(11/24): 벤치 실물(목 4 · 입구 16 mm) + 11/20 계량기 비교 시험의 유출계수 식 **C = a + b ÷ Re** (HSC a 0.97739 · b −158.91, Re 1,725 아래는 끝 값) — 벤치 서버 `bench/server/venturi.py` · 실시간 페이지 교정 표와 같은 식. Re = 4ρQ₀ ÷ (π dₜ μ), Q₀ 는 C 0.97 로 낸 유량. 가스 점성은 바이오가스(CH₄ 60 % · CO₂ 40 %) 문헌값을 Wilke 식으로 섞은 1.318e-5 Pa·s(20 °C) · Sutherland S 220 K. 맥 시험이 `venturi.py` 값 4점과 맞대 봄(공기). 계산식 문서 예제 1–6 시험은 예제 값(4.62 · 25 mm · C 0.97 고정)으로 따로 돌림.

HSC 영점 따라가기(11/24, PCB3_3): 벤치 STM 펌웨어(`bench/firmware`, `CONFIG_BENCH_HSC_TRACK_SDP`)와 같은 방식. SDP810 이 ±1 Pa 안(흐름 없음)이고 HSC − SDP810 이 ±5 Pa 안이면 영점을 차이 × (간격 ÷ τ)씩 SDP810 쪽으로 끌어감. 벤치는 0.1 s · τ 20 s, 보드는 평소 10 s 간격이라 τ 10분. 조리 중 · SDP810 없음(PCB3_2 · 설정 04 00)이면 멈춤. 따라간 영점은 메모리에만(다시 켜면 버튼 영점부터), 1분 로그 끝 `영점` 칸에 찍힘. 버튼 영점 ±1 Pa 밖으로는 안 감 — SDP810 관이 빠져 0 을 읽을 때 천천히 커지는 진짜 차압을 지우지 않게, 닿으면 로그로 알림(갑자기 큰 차압은 조리로 잡혀 원래 멈춤). C(Re) 가 0.5–1.05 밖이면 고정 C 0.97(벤치 venturi.py 와 같은 방어).

## 1분 보고 (`Core/Src/report.c`)

LoRa 로 1분마다 보내는 24바이트(형식 v1). 바이트 배치 표는 `Core/Inc/report.h` 맨 위. 서버 쪽 풀기 참고 구현은 `tools/report_decode.py`:

```bash
python3 tools/report_decode.py 0101020...   # 16진 문자열 하나 풀기
```

`./tests/host/run.sh` 가 C 로 묶은 값을 이 파이썬으로 풀어 맞는지도 확인함. 자릿수는 서버 담당과 맞출 때 바꿀 수 있고, 바꾸면 형식 버전을 올림.

## 센서 드라이버 (`Core/Src/hsc.c` · `abp2.c` · `bmp390.c` · `sensors.c`)

| 센서 | 버스 · 주소 | 하는 일 |
|---|---|---|
| HSC 차압 ±1000 Pa | I2C2 (PB10/PB12) 0x28 | 4바이트 읽기, 상태 2비트(10 = 이전 값) |
| ABP2 정압 0–6 kPa | I2C3 (PA8/PC9) 0x28 | `AA 00 00` → 5 ms → 7바이트, busy 면 1 ms 씩 다시 |
| BMP390 기압 · 온도 | I2C3 0x76 | 켤 때마다 다시 설정(OSR ×4/×1, IIR 끔), 강제 모드 한 번 재기, Bosch 실수 보정식 |

드라이버는 `i2c_bus_t`(쓰기 · 읽기 · 레지스터 읽기 · 기다리기) 만 써서 맥에서 가짜 버스로 시험함. 보드 쪽 연결은 `i2c_stm32.c`(HAL, 100 kHz, 타이밍은 버스 클럭에서 계산). `sensors_read()` 가 셋을 읽어 `meter_sample_t` 로 묶고, 기압 센서가 실패하면 마지막 기압 · 정압 센서 온도로 대신함. BMP390 보정식은 Bosch BMP3_SensorAPI 로 낸 기준값 14개와 맞춤(차이 0.0001 Pa 미만).

정압 범위는 우리 견적(ABP2 6 kPa) 기준. 동인님 시트는 0–2.5 kPa 라 합의 뒤 `abp2_default_cfg()` · `meter_default_cfg()` 만 바꾸면 됨.

## LoRa 모듈 (`Core/Src/rak3172.c` · `lora_uart_stm32.c`)

RAK3172(RUI3 AT)를 LPUART1(PB6/PB7)로 부림. 명령은 짧게 주고받고(`rak_cmd`, 1 s 이내), 가입 · 송신 결과는 나중에 오는 `+EVT:` 줄을 주 루프의 `rak_poll()` 이 처리함. 그래서 가입(최대 8번 × 10 s) · 송신 끝을 기다리는 동안에도 1초 측정이 멈추지 않음.

| 하는 일 | 함수 |
|---|---|
| 처음 한 번 설정 (LoRaWAN · OTAA · 대역 · 키 · LBT · 절전) — 지금 값과 다른 것만 씀 | `rak_configure` |
| 가입 `AT+JOIN=1:0:10:8` → `+EVT:JOINED` | `rak_join_start` · `rak_poll` |
| 1분 보고 `AT+SEND=포트:16진` → `+EVT:TX_DONE` | `rak_send_start` · `rak_poll` |
| 리셋(PE2 낮음 2 ms) · 깨우기(`AT` 최대 3번) · 속도 저장 `AT+BAUD=` | `rak_hw_reset` · `rak_wake` · `rak_set_baud` |

대역 기본은 국내 시험 KR920(`band = 7`, LBT 켬). 케냐 현장은 4(EU868) — 국내에서 868 MHz 로 쏘면 안 됨. 키(DevEUI · AppEUI · AppKey)는 `secrets.h` 에 두고 저장소에 안 올림. 깰 때 첫 글자를 잃는지 · 리셋 뒤 응답 시간 · LBT 시간 단위는 매뉴얼에 없어서 보드에서 확인.

## 1분 기록 저장 (`Core/Src/ring.c` · `w25q.c` · `flash_spi_stm32.c`)

외부 NOR 플래시 W25Q128(16 MB, SPI1)에 1분 기록을 32 B 칸으로 차례로 씀. 앞 12 MB = 393,216칸 = 273일(90일 이상), 위 4 MB 는 나중 용도로 비워 둠. 새 섹터(4 KB = 128칸)에 들어갈 때 그 섹터를 지우므로 가장 오래된 128분이 지워짐.

| 칸 32 B | 내용 |
|---|---|
| 0–3 | 순번(칸 번호와 같음, 작은 쪽 먼저) |
| 4–27 | 1분 보고 24 B (형식 v1) |
| 28 | 비트0: 1 = 안 보냄, 0 = 보냄 (지우지 않고 1 → 0 으로 고침) |
| 29 | 예비 FFh |
| 30–31 | CRC-16/CCITT-FALSE (0–27) |

켤 때 `ring_mount()` 가 섹터마다 첫 기록만 읽어 이어 쓸 자리를 찾음. 쓰다 끊긴 칸은 CRC 가 안 맞아 건너뛰고, 지우다 끊긴 섹터는 다음 쓰기 때 다시 지움. 마지막 기록(`ring_last`)으로 누적량도 되살릴 수 있음. 못 보낸 기록은 `ring_find_unsent()` 로 최근 몇 칸 안에서 가장 오래된 것부터 찾음.

## 잠 모드 (`Core/Src/lowpower.c` · `lowpower_stm32.c`)

평소엔 Stop 으로 자고 RTC 깨우기 타이머(LSE ÷ 16 = 2048 Hz)로 깸: 흐름 있으면 1 s, 없으면 10 s (설계서 결정 ①). 자는 동안 LPUART1 을 LSE 9600 으로 돌려 LoRa 모듈 글자가 오면 바로 깸 (결정 ⑦, BRR 874 · 오차 −213 ppm). LSE 로는 115200 을 못 내서, 처음 켤 때 `rak_ensure_baud(9600, 115200)` 가 모듈 속도를 9600 으로 저장함. 시각은 RTC 달력 ↔ Unix 초.

| 계산 부분 (맥 시험) | 보드 부분 |
|---|---|
| `lp_tick_ms` · `lp_wut_reload` · `lp_lpuart_brr` · 달력 ↔ Unix | `lp_init` (RTC · SVOS3 · 플래시 끔) · `lp_sleep_ms` · `lp_time_unix` |

보드에서 확인할 것: RTC 깨우기 EXTI 선 번호(17번으로 둠, RM0481 표 확인) · LPUART1 이 Stop 에서 깨우는지 · 잠 전류. 디버거를 붙인 채로 잠을 보려면 `LP_DEBUG_STOP` 을 켜고 빌드.

## 누적 · 설정 · 키 저장 (`Core/Src/nvlog.c` · `persist.c`)

정전 뒤에도 남길 값 두 묶음을 "마지막에 쓴 것이 이김" 기록으로 저장. 섹터 두 개를 번갈아 쓰므로 지우다 끊겨도 앞 값이 남음.

| 묶음 | 내용 | 언제 |
|---|---|---|
| 설정 | 영점 · 방출 문턱 · LoRa 대역 · 가입 키(DevEUI · AppEUI · AppKey) | 영점 버튼 · 키 넣을 때 |
| 계수 | 누적 사용량(표준 · 실제) · 보고 순번 · 방출 분 · 고갈 · 이벤트 수 | 1분마다 (섹터 수명 약 24년) |

보드에서는 MCU 내장 고내구 데이터 플래시(EDATA, 은행 1 섹터 124–127 = 0x0900_6000~, 섹터 6 KB · 10만 회)에 둠 (`nv_edata.c`). 처음 켤 때 옵션 바이트로 EDATA 를 켜고, 캐시 안 하게 MPU 를 설정함. 한 번도 안 쓴 칸을 읽으면 ECC 이중 오류(NMI)가 나는데, NMI 에서 지우고 돌아가며 `nvlog` 는 빈 칸의 머리 8 B 만 읽어 그 횟수를 줄임. 근거: RM0481 7장(1 MB 판도 EDATA 는 섹터 120–127, 코드 섹터와 안 겹침) · ST 예제 `FLASH_EDATA_EraseProgram` · ST 커뮤니티 답. EDATA 를 못 켜면 외부 플래시 위 4 MB(0xC00000~)로 대신함. 1분마다 써도 섹터 수명 약 36년(96칸 × 2섹터 × 10만 회).

키는 `Core/Inc/secrets.example.h` 를 `secrets.h` 로 복사해 넣고 빌드하면 처음 켤 때 설정에 저장됨 (`secrets.h` 는 저장소에 안 올라감).

## 전체 흐름 (`Core/Src/app.c` · `main.c`)

켜기: 시계(LSE · RTC) → 센서 → 외부 플래시(링 · 저장) → 저장값 되살리기 → LoRa(9600 · 설정 · 가입). 그 뒤 한 번 깰 때마다 `app_step()`:

1. 센서 읽기 → 계산
2. LoRa 소식 처리 (송신 끝 → 링에 "보냄" 표시)
3. 1분이 넘었으면 1분 보고 → 링 저장 → 계수 저장 → 바로 보냄 (포트 1)
4. 보낼 게 없으면 최근 60분 안의 못 보낸 기록을 20초에 하나씩 다시 보냄 ("다시 보냄" 비트)
5. 가입 안 됐으면 10분마다 다시 가입

그리고 Stop 으로 잠 (흐름 있으면 1 s, 없으면 10 s). 버튼을 3초 누르면 그 전 16번 읽은 차압 평균으로 영점을 잡고 저장(LED1 세 번). `tests/host/test_app.c` 가 가짜 센서 · 시계 · 플래시 · LoRa 모듈로 이 흐름을 통째로 돌려 봄(전원 끊김 · 저장 영역 망가짐 · 센서 고장 · 배터리 낮음 포함).



## 셀 전압 (`Core/Src/batt.c` · `batt_stm32.c`)

1분 보고 직전(LoRa 송신 전 쉬는 상태)에 ADC1 로 PA0(셀 ÷ 2)과 내부 기준 VREFINT 를 16번씩 재 평균함. 3V3 가 ±1.5 % 흔들려도 VREFINT 로 VDDA 를 구해 환산하므로 그 오차가 빠짐(3.25 V 일 때 안 쓰면 +57 mV). 생산 때 1점 보정값(`settings.batt_cal_mv`)을 더함. 재고 나면 ADC 를 끔.

## 시계 맞추기 (`rak_time_request` · `rak_get_time` · `app.c`)

가입하면 `AT+TIMEREQ=1` 로 다음 송신 때 망에 시각을 묻고, 송신이 끝날 때마다 `AT+LTIME=?` 를 읽음(답 예: `LTIME: 04h36m00s on 11/27/2023`, UTC+0). 연도가 2026–2099 인 답이 오면 2초 넘게 어긋났을 때 RTC 를 고치고 `AT+TIMEREQ=0`, 하루 뒤 다시 맞춤. 시계가 뛰면 적산 기준 · 분 경계도 같이 옮겨 큰 간격을 적산하지 않음(한 번 적산은 최대 60 s). MCU 만 리셋될 땐 RTC 를 다시 초기화하지 않아 맞춘 시각이 이어짐.

## 처음 한 번

```bash
git submodule update --init --depth 1
```

`third_party/` 에 ST 코드가 들어옴 (cmsis-device-h5 v1.7.0 · stm32h5xx-hal-driver v1.7.0 · cmsis_core v5.9.0).

## 빌드

```bash
source ~/toolchains/env.sh
cmake -S . -B build.nosync -G Ninja
cmake --build build.nosync
```

빌드 폴더 이름을 `build.nosync` 로 두면 iCloud 가 동기화하지 않음. 한 번에 두 판이 다 나옴: `build.nosync/gasmeter_pcb3_2.elf` · `gasmeter_pcb3_3.elf` (+ `.hex` · `.bin` · `.map`). 한 판만은 `--target gasmeter_pcb3_3.elf`.

## 판별 폴더 (`boards/`)

| 폴더 | 판 | 상태 | 다른 점 |
|---|---|---|---|
| `boards/pcb3_2/` | PCB3_2 | 11/18 JLC 주문 · 만드는 중 | 기준 판. PB13 · PA9 빈 핀 |
| `boards/pcb3_3/` | PCB3_3 | 11/24 거버 · 견적만 | J_SDP(SDP810, I2C3 0x25, 선으로) · J_TMP(DS18B20, PB13) · PA9 → GND |

- `board.h`: 판 이름(`BOARD_NAME`, 켤 때 로그 첫 줄) · 있는 센서(`BOARD_HAS_SDP810` · `BOARD_HAS_DS18B20`) · `BOARD_PA9_GND`. 그 밖은 `pins.h` 를 부름.
- `pins.h`: MCU 신호 핀. PCB3_3 것은 부품 구성 v0.29 의 U1 핀 넷과 대조해 차이 0.
- PCB3_2 빌드는 SDP810 주소(0x25)와 PB13 을 아예 안 건드림 → 보고 24 · 26 번은 늘 0x8000. 보고 형식(v2 28 B)은 두 판 같음 — 서버 · ChirpStack 코덱은 하나.
- 링커 스크립트는 두 판 같음: `boards/STM32H563ZGTX_FLASH.ld`.
- 새 판이 생기면 `boards/<새 판>/` 에 두 파일을 만들고 CMakeLists.txt 에 `add_gasmeter(<새 판>)` 한 줄.

## 넣기 (STLINK-V3SET → J_SWD)

케이블 빨간 줄을 J_SWD 의 네모 패드(1번) 쪽으로 꽂음.

```bash
./tools/flash.sh                                   # PCB3_2 (기본)
./tools/flash.sh build.nosync/gasmeter_pcb3_3.elf  # PCB3_3
```

로그 보기 (Serial Basic → J_DBG, 3.3 V):

```bash
pyserial-miniterm /dev/cu.usbserial-XXXX 115200
```

## 메모리

- 내장 플래시 1 MB 중 은행 1(512 KB, 0x0800_0000~)만 씀. 은행 2 는 나중에 원격 갱신을 넣을 자리로 비워 둠(설계서 9번).
- 링커 스크립트: `boards/STM32H563ZGTX_FLASH.ld` (ST 템플릿에서 FLASH 길이만 512K 로 바꿈)
