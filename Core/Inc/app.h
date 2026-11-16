/* 계량기 전체 흐름 — 센서 → 계산 → 1분 기록 저장 → LoRa 보고 → 잠 (설계서 "동작 흐름" 그림)
 *
 * 하드웨어는 app_hw_t 콜백과 이미 열어 둔 링 · 저장 · LoRa 모듈로만 만짐 → 맥에서 가짜 부품으로 통째로 시험함.
 *
 * 한 번 깰 때마다 app_step():
 *   1) 센서 읽기 → 계산(meter_step)
 *   2) LoRa 모듈 소식 처리 (가입됨 · 송신 끝 → 링에 "보냄" 표시)
 *   3) 1분이 넘었으면: 1분 요약 → 24 B 보고 → 링에 저장 → 누적량 저장 → 바로 보냄
 *   4) 보낼 게 없으면 최근 60분 안의 못 보낸 기록을 하나 다시 보냄 (20 s 에 한 번)
 *   5) 가입 안 돼 있으면 10분마다 다시 가입
 *   → 다음에 깰 때까지 ms (흐름 있으면 1 s, 없으면 10 s)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "meter.h"
#include "nvlog.h"
#include "persist.h"
#include "rak3172.h"
#include "report.h"
#include "ring.h"

#define APP_LORA_PORT        1u      /* 1분 보고 형식 v1 */
#define APP_RESEND_WINDOW    60u     /* 최근 60분 기록까지 다시 보냄 */
#define APP_RESEND_GAP_S     20u
#define APP_JOIN_RETRY_S     600u
#define APP_ZERO_SAMPLES     16u     /* 영점 = 최근 16번 읽은 차압 평균 */
#define APP_LORA_BAUD        9600u   /* 잠 중에도 받는 속도 (설계서 결정 ⑦) */
#define APP_LORA_BAUD_OLD    115200u /* 모듈 공장값 */

typedef struct {
    bool (*read)(void *ctx, meter_sample_t *s);   /* 센서 읽기, 믿을 만하면 true */
    uint32_t (*now_s)(void *ctx);                  /* RTC Unix 초 */
    bool (*time_valid)(void *ctx);                 /* 시계를 맞췄나 */
    uint16_t (*batt_mv)(void *ctx);                /* 셀 전압, 0 = 아직 못 잼 */
    void (*flash_power)(void *ctx, bool on);       /* 외부 플래시 깨우기 · 재우기 (없으면 NULL) */
    void (*log)(void *ctx, const char *msg);       /* 없으면 NULL */
    void *ctx;
} app_hw_t;

typedef struct {
    app_hw_t hw;
    meter_t meter;
    ring_t *ring;
    nvlog_t *nv_set, *nv_ctr;
    rak_t *rak;
    settings_t settings;
    counter_t counter;
    bool lora_ready;            /* 속도 · 설정까지 끝남 */
    bool started;
    uint32_t last_s;            /* 지난 app_step 시각 */
    uint32_t last_minute;       /* 마지막으로 보고 만든 분 번호 (Unix 초 / 60) */
    uint16_t batt_flags;        /* 배터리 비트 오르내림 폭용 */
    bool fresh_pending;         /* 방금 만든 보고를 아직 안 보냄 */
    uint32_t fresh_seq;
    bool tx_live;               /* 모듈이 보내는 중인 기록 */
    uint32_t tx_seq;
    uint32_t next_join_s, next_resend_s;
    double dp_hist[APP_ZERO_SAMPLES];
    uint16_t dp_n, dp_head;
    /* 셈 (로그 · 시험용) */
    uint32_t reports, sent, send_fail, resends;
} app_t;

/* 저장된 설정 · 누적량을 읽어 계산 모듈에 넣음. 누적 기록이 없으면 링 마지막 보고에서 되살림. */
void app_init(app_t *a, const app_hw_t *hw, ring_t *ring, nvlog_t *nv_set, nvlog_t *nv_ctr, rak_t *rak);
/* LoRa: 속도 9600 맞추기 → 키가 있으면 설정 · 가입 시작. 키가 없으면 기록만 쌓음. */
rak_err_t app_lora_start(app_t *a);
/* 한 번 깸. 다음에 깰 때까지 ms 를 돌려줌. */
uint32_t app_step(app_t *a);
/* 버튼 길게: 지금 차압 평균을 영점으로 · 저장 */
bool app_zero(app_t *a);
/* 키 넣기 (처음 설치 때 · secrets.h 에서) — 저장 뒤 다시 설정 · 가입 */
bool app_set_keys(app_t *a, const uint8_t dev_eui[8], const uint8_t app_eui[8], const uint8_t app_key[16]);
