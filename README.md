# Firmware

가스 계량기(GasMeter_19, PCB3_2) 펌웨어. MCU 는 STM32H563ZGT6.

- 설계서(하는 일 · 정한 것 9가지 · 보드 오면 하는 순서): https://claude.ai/artifact/4pURFGYAHrpZGocGMBt1hs
- 핀: `Core/Inc/pins.h` (보드 원본에서 뽑은 것, PCB-Sampton/Docs/GasMeter_19_pins.h 와 같음)

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

판정 문턱과 벤투리 치수는 문서의 제안값 · 예제 값이라 `meter_default_cfg()` 에 설정값으로 모여 있음.

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

빌드 폴더 이름을 `build.nosync` 로 두면 iCloud 가 동기화하지 않음. 결과물은 `build.nosync/gasmeter.elf` · `.hex` · `.bin`.

## 넣기 (STLINK-V3SET → J_SWD)

케이블 빨간 줄을 J_SWD 의 네모 패드(1번) 쪽으로 꽂음.

```bash
./tools/flash.sh
```

로그 보기 (Serial Basic → J_DBG, 3.3 V):

```bash
pyserial-miniterm /dev/cu.usbserial-XXXX 115200
```

## 메모리

- 내장 플래시 1 MB 중 은행 1(512 KB, 0x0800_0000~)만 씀. 은행 2 는 나중에 원격 갱신을 넣을 자리로 비워 둠(설계서 9번).
- 링커 스크립트: `board/STM32H563ZGTX_FLASH.ld` (ST 템플릿에서 FLASH 길이만 512K 로 바꿈)
