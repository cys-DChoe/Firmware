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
