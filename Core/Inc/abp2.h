/* Honeywell ABP2 정압 센서 (ABP2DRNT006KG2A3XX, 0–6 kPa 게이지, I2C 0x28) — U-S2, I2C3
 *
 * ABP2 데이터시트 6.4–6.6 · Table 6 · 22 · 23:
 *   측정 명령: 0xAA 0x00 0x00 쓰기 → 대기(Standby)에서 깨어 한 번 재고 다시 잠
 *   기다리기: 상태 바이트의 busy 비트가 풀릴 때까지, 또는 5 ms, 또는 EOC 핀(PB14)
 *   읽기 7바이트: 상태 · 압력 24비트 · 온도 24비트
 *   상태 비트: 6 = 전원 켜짐(1) · 5 = busy · 2 = 메모리 검사 실패 · 0 = 계산 포화
 *   압력: 10 % = 1677722, 90 % = 15099494 (2²⁴ 기준)
 *   온도 °C = 출력 × 200 ÷ 16777215 − 50
 * 전원 넣고 첫 명령까지 2.5 ms (3.0절).
 */
#pragma once

#include <stdbool.h>
#include "i2c_bus.h"

#define ABP2_ADDR        0x28u
#define ABP2_OUT_MIN     1677722.0
#define ABP2_OUT_MAX     15099494.0
#define ABP2_STARTUP_MS  3u
#define ABP2_CONV_MS     5u

#define ABP2_ST_POWER    (1u << 6)
#define ABP2_ST_BUSY     (1u << 5)
#define ABP2_ST_MEMERR   (1u << 2)
#define ABP2_ST_SAT      (1u << 0)

typedef struct {
    double p_min_pa;        /* 0 Pa */
    double p_max_pa;        /* 6000 Pa (006KG) */
} abp2_cfg_t;

typedef struct {
    uint8_t status;
    uint32_t p_counts;
    uint32_t t_counts;
    double p_pa;
    double t_c;
} abp2_reading_t;

void abp2_default_cfg(abp2_cfg_t *cfg);

/* 측정 시작 (0xAA 0x00 0x00) */
sens_err_t abp2_start(const i2c_bus_t *bus);
/* 결과 읽기 — 아직 변환 중이면 SENS_E_BUSY */
sens_err_t abp2_fetch(const i2c_bus_t *bus, const abp2_cfg_t *cfg, abp2_reading_t *out);
/* 시작 → 5 ms 기다림 → 읽기 (busy 면 1 ms 씩 몇 번 더) */
sens_err_t abp2_measure(const i2c_bus_t *bus, const abp2_cfg_t *cfg, abp2_reading_t *out);

sens_err_t abp2_decode(const uint8_t raw[7], const abp2_cfg_t *cfg, abp2_reading_t *out);
