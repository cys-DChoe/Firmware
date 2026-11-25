/* Sensirion SDP810-500Pa 차압 — 다음 판(PCB3_3) J_SDP 헤더에 선으로 꽂음, I2C3(ABP2 · BMP390 과 같은 버스) 0x25
 *
 * SDP8xx-Digital 데이터시트 v1.1 (04/2019):
 *   트리거 측정 0x362F (차압 온도 보상 · 클럭 늘림 없음) → 45 ms(40–50) 뒤 9바이트 읽기   6.3.3절
 *     차압 2 + CRC · 온도 2 + CRC · 눈금 2 + CRC  (CRC-8: 다항식 0x31, 처음 0xFF)       6.3.3 · 6.4절
 *     차압 Pa = 값 ÷ 눈금(500 Pa 품번 60), 온도 °C = 값 ÷ 200
 *   잠 0x3677 (측정 끝난 쉼 상태에서만) — 1 µA                                         6.3.5절
 *     깨우기 = 주소 + 쓰기. 그때 센서는 답(ACK)하지 않음, 2 ms 안에 깸
 *   측정 중엔 읽기 머리에 답하지 않음 — 그래서 묻지 않고 50 ms 기다린 뒤 한 번 읽음
 *     (안 그러면 답 없음이 I2C 실패로 쌓여 버스를 다시 열게 됨, i2c_stm32.c I2C_FAILS_REOPEN)
 *
 * 쓰는 순서 (sensors.c): sdp810_trigger → (ABP2 · HSC · BMP390 읽는 동안) → 50 ms 채움 → sdp810_fetch → sdp810_sleep
 * 센서가 없는 판(PCB3_2)에서는 SDP810_RETRY_READS 번 읽을 때마다 한 번만 찾아봄 — 같은 버스의 다른 센서에 영향 없게.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "i2c_bus.h"
#include "sdp810_comp.h"   /* 대기압 보정 — 벤치(bench/nucleo_lora)와 같이 씀 */

#define SDP810_ADDR         0x25u
#define SDP810_MEAS_MS      50u     /* 트리거 측정 최대 50 ms */
#define SDP810_WAKE_MS      2u      /* 잠에서 깨는 시간 */
#define SDP810_RETRY_READS  60u     /* 없으면 이만큼 읽을 때마다 한 번 다시 찾음 (조리 중 1분 · 쉴 때 10분) */

typedef struct {
    bool present;           /* 마지막으로 답한 적이 있음 */
    bool started;           /* 이번 읽기에서 측정을 시작함 */
    uint16_t skip;          /* 없을 때 다시 찾기까지 남은 읽기 수 */
    int16_t scale;          /* 마지막 눈금 (500 Pa 품번 = 60) */
    uint32_t fails;         /* 켠 뒤 실패 수 (로그용) */
} sdp810_t;

typedef struct {
    double dp_pa;
    double t_c;
} sdp810_reading_t;

void sdp810_init(sdp810_t *d);

/* 측정 시작. 잠들어 있으면 깨워서(답 없음 → 2 ms → 다시) 시작. 시작했으면 SENS_OK.
 * 없다고 본 동안엔 버스를 건드리지 않고 SENS_E_BUS (SDP810_RETRY_READS 번마다 한 번 시도) */
sens_err_t sdp810_trigger(const i2c_bus_t *bus, sdp810_t *d);

/* 시작하고 SDP810_MEAS_MS 지난 뒤 부름. CRC 가 맞으면 SENS_OK */
sens_err_t sdp810_fetch(const i2c_bus_t *bus, sdp810_t *d, sdp810_reading_t *out);

/* 잠 (1 µA). 측정을 시작했던 때만 보냄 */
void sdp810_sleep(const i2c_bus_t *bus, sdp810_t *d);

/* 맥 시험용 */
uint8_t sdp810_crc8(const uint8_t *p);
