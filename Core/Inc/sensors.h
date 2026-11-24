/* 센서를 한 번에 읽어 계산 모듈(meter)에 넣을 값으로 만듦
 *   차압: HSC (I2C2 단독) · 정압: ABP2 (I2C3) · 기압 · 온도: BMP390 (I2C3)
 *   다음 판(PCB3_3) 추가: 차압 비교 SDP810 (I2C3, 선으로) · 가스 온도 DS18B20 (1-Wire PB13)
 *     — 없는 판에서도 같은 펌웨어로 돎(없으면 1분 보고 24 · 26 번이 0x8000)
 * 센서 전원은 SENS_EN(PE3) — 설계서 1번: 평소엔 끄는 것이 목표라 켜고 끄기를 따로 둠.
 */
#pragma once

#include <stdbool.h>
#include "abp2.h"
#include "bmp390.h"
#include "hsc.h"
#include "meter.h"
#include "onewire.h"
#include "sdp810.h"

/* DS18B20: 이 간격마다 변환 시작(750 ms · 1.5 mA → 평균 약 0.1 mA), 결과는 750 ms 넘게 지난 다음 읽기에서 */
#define SENSORS_PROBE_PERIOD_MS  10000u
/* 마지막 정상값이 이보다 오래되면 가스 온도를 BMP390 으로 되돌림 */
#define SENSORS_PROBE_MAX_AGE_MS 60000u

/* 전원을 켠 뒤 기다리는 시간: 센서 시작 시간(HSC 3 · ABP2 2.5 · BMP390 2 ms)에
 * 센서 전원(U9 승압) 안정 여유를 더함 — 보드에서 재고 줄일 수 있음 */
#define SENSORS_POWER_ON_MS  15u
/* 기압 센서(BMP390)가 설정 안 된 채면 이만큼 읽을 때마다 다시 설정해 봄 (평소 10 s × 6 = 1분, 조리 중 6 s) */
#define SENSORS_BMP_RETRY    6u

typedef struct {
    const i2c_bus_t *bus_dp;        /* I2C2: HSC */
    const i2c_bus_t *bus_pg;        /* I2C3: ABP2 · BMP390 */
    void (*power)(bool on);         /* SENS_EN */
    hsc_cfg_t hsc;
    abp2_cfg_t abp2;
    bmp390_t bmp;
    bool powered;
    bool bmp_ready;
    uint8_t bmp_retry_n;            /* 설정 안 된 채 읽은 횟수 (다시 설정해 볼 때까지) */
    uint32_t bmp_reinits;           /* 다시 설정해서 살아난 횟수 (로그용) */
    double patm_fallback_pa;        /* 기압 센서 실패 때 쓸 값 (마지막 정상값, 처음엔 해수면 101,325 Pa) */
    /* 다음 판 센서 */
    sdp810_t sdp;
    const onewire_t *ow;            /* NULL 이면 DS18B20 안 씀 */
    uint32_t (*tick_ms)(void);      /* 깨어 있을 때 ms (SDP810 50 ms 채우기). NULL 이면 50 ms 를 다 기다림 */
    uint32_t (*wall_ms)(void);      /* 잠 동안에도 흐르는 ms (DS18B20 간격 · 나이). NULL 이면 DS18B20 안 씀 */
    bool probe_pending;             /* 변환 중 */
    bool probe_started_once;
    bool probe_have;                /* 정상값을 한 번이라도 읽음 */
    uint32_t probe_start_ms, probe_at_ms;
    double probe_t_c;
} sensors_t;

typedef struct {
    meter_sample_t sample;          /* 계산 모듈 입력 */
    sens_err_t e_dp, e_pg, e_baro;
    double t_hsc_c, t_abp2_c;       /* 센서 자체 온도 (참고) */
    double t_board_c;               /* BMP390 칩 온도 = 함체 안 (참고, 기압 실패면 0) */
    sens_err_t e_sdp, e_probe;      /* 다음 판 센서 (없으면 SENS_E_BUS) */
    double t_sdp_c;                 /* SDP810 칩 온도 (참고) */
    bool usable;                    /* 차압 · 정압을 믿을 수 있어 meter_step 에 넣어도 됨 */
    bool clipped;                   /* 차압 · 정압이 센서 범위를 벗어나 끝값으로 자름 */
} sensors_result_t;

void sensors_init(sensors_t *s, const i2c_bus_t *bus_dp, const i2c_bus_t *bus_pg, void (*power)(bool on));
/* 다음 판 센서 연결: ow = DS18B20 1-Wire(NULL 이면 안 씀),
 * tick_ms = 깨어 있을 때 정확한 ms(보드 HAL_GetTick), wall_ms = 잠 동안에도 흐르는 ms(보드 RTC) */
void sensors_set_extra(sensors_t *s, const onewire_t *ow, uint32_t (*tick_ms)(void), uint32_t (*wall_ms)(void));
sens_err_t sensors_power_on(sensors_t *s);
void sensors_power_off(sensors_t *s);   /* SENS_EN 만 내림 = HSC 레일. ABP2 · BMP390 은 3V3 상시 */

/* 한 번 읽기 (약 17 ms: ABP2 5 ms + BMP390 11 ms. SDP810 이 있으면 그 측정 50 ms 까지 늘어남).
 * 가스 온도: DS18B20 이 있으면 그 값, 없으면 BMP390 온도(함체 안 온도라 근사값 — 계산식 문서 "우리 부품" 메모).
 * SDP810 은 매번 깨워 재고 다시 재움(잠 1 µA). 값은 비교용으로 sample.dp2_pa 에만 — 사용량 계산은 HSC 로.
 * 기압이 실패하면 마지막 정상 기압으로 대신하고 e_baro 와 sample.patm_held 에 남김.
 * 켤 때 기압 센서가 답이 없었으면 SENSORS_BMP_RETRY 번 읽을 때마다 다시 설정해 봄 (11/18 검수:
 * 전엔 한 번 실패하면 계속 해수면 값으로 계산 — 나이로비 높이면 사용량 +11 %). */
void sensors_read(sensors_t *s, sensors_result_t *r);
