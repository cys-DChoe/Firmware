/* Honeywell HSC 차압 센서 (HSCDRRN010MD2A3, ±10 mbar, I2C 0x28) — U-S1, I2C2
 *
 * HSC 데이터시트 Table 7: 디지털 출력 10 % = 1638, 90 % = 14746 (14비트)
 * Honeywell I2C 기술 노트: 4바이트 읽기 = 상태 2비트 + 압력 14비트 + 온도 11비트
 *   상태 00 정상 · 01 명령 모드 · 10 이전 값(stale) · 11 진단 오류
 *   온도 °C = 출력 × 200 ÷ 2047 − 50
 * 전원 넣고 첫 값까지 3 ms (Table 6). 따로 측정 명령 없음 — 계속 재고 있음.
 */
#pragma once

#include "i2c_bus.h"

#define HSC_ADDR        0x28u
#define HSC_OUT_MIN     1638.0
#define HSC_OUT_MAX     14746.0
#define HSC_STARTUP_MS  3u

typedef struct {
    double p_min_pa;        /* −1000 Pa (±10 mbar) */
    double p_max_pa;        /* +1000 Pa */
} hsc_cfg_t;

typedef struct {
    uint8_t status;         /* 0–3 */
    uint16_t p_counts;      /* 14비트 */
    uint16_t t_counts;      /* 11비트 */
    double p_pa;
    double t_c;
} hsc_reading_t;

void hsc_default_cfg(hsc_cfg_t *cfg);
sens_err_t hsc_read(const i2c_bus_t *bus, const hsc_cfg_t *cfg, hsc_reading_t *out);

/* 바이트 → 값 (시험용으로 따로 둠) */
sens_err_t hsc_decode(const uint8_t raw[4], const hsc_cfg_t *cfg, hsc_reading_t *out);
