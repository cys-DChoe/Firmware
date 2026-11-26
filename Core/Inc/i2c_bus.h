/* I2C 버스 틀 — 센서 드라이버는 이것만 보고, 실제 버스는 보드(i2c_stm32.c) 또는 맥 시험(가짜 버스)이 채움 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct i2c_bus {
    /* 0 = 성공, 음수 = 실패 (응답 없음 · 타임아웃) */
    int (*write)(void *ctx, uint8_t addr7, const uint8_t *data, size_t len);
    int (*read)(void *ctx, uint8_t addr7, uint8_t *data, size_t len);
    /* 레지스터 주소를 쓰고 이어서 읽기 (BMP280) */
    int (*write_read)(void *ctx, uint8_t addr7, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} i2c_bus_t;

/* 센서 드라이버 공통 결과 */
typedef enum {
    SENS_OK = 0,
    SENS_STALE = 1,         /* 값은 있으나 새로 잰 값이 아님 (HSC 상태 10) */
    SENS_E_BUS = -1,        /* I2C 응답 없음 */
    SENS_E_ID = -2,         /* 칩 번호가 다름 */
    SENS_E_BUSY = -3,       /* 아직 변환 중 */
    SENS_E_STATUS = -4,     /* 센서가 오류를 알림 (진단 · 메모리 · 포화) */
    SENS_E_RANGE = -5,      /* 값이 센서 범위 밖 (잘림) */
} sens_err_t;
