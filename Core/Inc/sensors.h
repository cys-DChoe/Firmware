/* 센서 3종을 한 번에 읽어 계산 모듈(meter)에 넣을 값으로 만듦
 *   차압: HSC (I2C2 단독) · 정압: ABP2 (I2C3) · 기압 · 온도: BMP390 (I2C3)
 * 센서 전원은 SENS_EN(PE3) — 설계서 1번: 평소엔 끄는 것이 목표라 켜고 끄기를 따로 둠.
 */
#pragma once

#include <stdbool.h>
#include "abp2.h"
#include "bmp390.h"
#include "hsc.h"
#include "meter.h"

/* 전원을 켠 뒤 기다리는 시간: 센서 시작 시간(HSC 3 · ABP2 2.5 · BMP390 2 ms)에
 * 센서 전원(U9 승압) 안정 여유를 더함 — 보드에서 재고 줄일 수 있음 */
#define SENSORS_POWER_ON_MS  15u

typedef struct {
    const i2c_bus_t *bus_dp;        /* I2C2: HSC */
    const i2c_bus_t *bus_pg;        /* I2C3: ABP2 · BMP390 */
    void (*power)(bool on);         /* SENS_EN */
    hsc_cfg_t hsc;
    abp2_cfg_t abp2;
    bmp390_t bmp;
    bool powered;
    bool bmp_ready;
    double patm_fallback_pa;        /* 기압 센서 실패 때 쓸 값 (마지막 정상값) */
} sensors_t;

typedef struct {
    meter_sample_t sample;          /* 계산 모듈 입력 */
    sens_err_t e_dp, e_pg, e_baro;
    double t_hsc_c, t_abp2_c;       /* 센서 자체 온도 (참고) */
    bool usable;                    /* 차압 · 정압을 믿을 수 있어 meter_step 에 넣어도 됨 */
    bool clipped;                   /* 차압 · 정압이 센서 범위를 벗어나 끝값으로 자름 */
} sensors_result_t;

void sensors_init(sensors_t *s, const i2c_bus_t *bus_dp, const i2c_bus_t *bus_pg, void (*power)(bool on));
sens_err_t sensors_power_on(sensors_t *s);
void sensors_power_off(sensors_t *s);

/* 한 번 읽기 (약 17 ms: ABP2 5 ms + BMP390 11 ms).
 * 가스 온도는 BMP390 온도를 씀 — 계산식 문서 "우리 부품" 메모: 함체 안 온도라 근사값임.
 * 기압이 실패하면 마지막 정상 기압으로 대신하고 e_baro 에 남김. */
void sensors_read(sensors_t *s, sensors_result_t *r);
