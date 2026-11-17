/* LTE 모듈 EG915U-EU — 1차 범위: 켜고 · 망 접속 확인 · 끄기 (설계서 5번, 11/17)
 *
 * 근거: EG915U Hardware Design(인쇄 38–44쪽) · Docs/GasMeter_19_펌웨어_주의.html 1–6절
 *   켜기: PWRKEY 2 s 이상 누름 → VDD_EXT(=MCU VDDIO2) 약 1.15 s · UART 약 4 s · STATUS 약 5 s 뒤 올라옴
 *   끄기: AT+QPOWD=1, 안 되면 PWRKEY 3 s 이상 → STATUS 가 낮아질 때까지 최대 30 s 쯤
 *   RESET_N 은 위 둘로 안 꺼질 때만 (리셋하면 모듈이 다시 켜지므로 그 뒤 한 번 더 끔)
 *   1.8 V 핀(UART · STATUS)은 VDD_EXT 가 올라온 뒤에 설정, 끄기 전에 아날로그로
 *   켜는 동안은 셀이 3.6 V 이상일 때만 (부르는 쪽이 확인)
 *
 * 한 번에 오래 막지 않도록 단계로 나눔: lte_start() 뒤 lte_step() 을 1 s 마다 부르면 됨.
 * 명령 하나는 최대 1 s 기다리고, 긴 기다림(켜짐 · 가입 · 꺼짐)은 부르는 쪽이 주는 초로 잼(Stop 중 HAL 틱 멈춤).
 * 그 초는 "켜진 뒤 흐른 초"처럼 뛰지 않는 것이어야 함 — RTC 시각을 그대로 주면 망 시각으로 시계를 맞출 때
 * 기다림이 한꺼번에 끝나, 꺼지는 중인 모듈에 PWRKEY 를 눌러 다시 켤 수 있음 (11/17 검수).
 * 그동안 측정 · LoRa 는 그대로 돎. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LTE_KEY_ON_S        3u      /* PWRKEY 누름 (최소 2 s, RTC 초 눈금이라 3 으로 — 실제 2–3 s) */
#define LTE_KEY_OFF_S       4u      /* PWRKEY 끄기 누름 (최소 3 s) */
#define LTE_STATUS_ON_S     15u     /* STATUS 가 올라오길 기다림 (보통 5 s) */
#define LTE_IO_S            5u      /* VDDIO2 준비를 기다림 */
#define LTE_AT_S            15u     /* "AT" 대답을 기다림 */
#define LTE_SIM_S           10u
#define LTE_REG_S           180u    /* 망 등록을 기다림 [가정 — 현장에서 다시 정함] */
#define LTE_OFF_S           35u     /* 꺼짐을 기다림 (데이터시트 30 s 쯤) */
#define LTE_CMD_MS          1000u
#define LTE_LINE_MAX        128u

typedef enum {
    LTE_R_OK = 0,           /* 망에 등록됨 */
    LTE_R_NO_POWER,         /* STATUS 가 안 올라옴 — 모듈이 안 켜짐 */
    LTE_R_NO_IO,            /* VDDIO2(1.8 V)가 준비 안 됨 */
    LTE_R_NO_AT,            /* AT 대답 없음 */
    LTE_R_NO_SIM,           /* 유심 없음 · 안 읽힘 */
    LTE_R_NO_NET,           /* 제한 시간 안에 망 등록 안 됨 */
    LTE_R_ABORTED,          /* 중간에 멈춤 (셀 낮음 등) */
    LTE_R_LEFT_ON,          /* 시키지 않았는데 켜져 있어서 끔 (MCU 만 다시 켜진 뒤 · 지난번에 못 끔) */
} lte_code_t;

typedef enum {
    LTE_OFF_NONE = 0,       /* 켜진 적 없음 */
    LTE_OFF_QPOWD,          /* AT+QPOWD 로 꺼짐 */
    LTE_OFF_KEY,            /* PWRKEY 로 꺼짐 */
    LTE_OFF_RESET,          /* 리셋 뒤 PWRKEY 로 꺼짐 */
    LTE_OFF_FAILED,         /* 못 끔 — 셀을 계속 씀 */
} lte_off_t;

typedef struct {
    lte_code_t code;
    lte_off_t off;
    uint8_t reg;            /* +CEREG stat: 1 = 집 망, 5 = 로밍, 2 = 찾는 중, 3 = 거절, 0 = 안 찾음 */
    uint8_t csq;            /* +CSQ rssi 0–31 (99 = 모름) — dBm = −113 + 2 × csq */
    char oper[20];          /* +COPS 이름 */
    uint16_t secs;          /* 켜서 끌 때까지 */
} lte_result_t;

typedef struct {
    int (*write)(void *ctx, const char *s, size_t n);
    int (*getc)(void *ctx);                 /* 받은 글자 없으면 −1 */
    uint32_t (*ms)(void *ctx);
    void (*pwrkey)(void *ctx, bool press);  /* true = 누름 (MCU PB5 높음 → NPN → PWRKEY 낮음) */
    void (*rst)(void *ctx, bool assert);    /* true = 리셋 (PG15 높음 → RESET_N 낮음) */
    bool (*status)(void *ctx);              /* STATUS (PB9) 높음 = 켜져 있음 */
    bool (*io_on)(void *ctx, bool on);      /* on: VDDIO2 준비됐으면 UART · 핀 켜고 true / off: 핀 아날로그 */
    void *ctx;
} lte_io_t;

typedef enum {
    LTE_S_IDLE = 0,
    LTE_S_PRESS, LTE_S_WAIT_STATUS, LTE_S_IO, LTE_S_AT, LTE_S_SIM, LTE_S_REG, LTE_S_INFO,
    LTE_S_QPOWD, LTE_S_WAIT_OFF, LTE_S_KEY_OFF, LTE_S_RESET,
} lte_state_t;

typedef struct {
    lte_io_t io;
    lte_state_t state;
    uint32_t t_state, t_start;
    bool abort, reset_done, done;   /* done: 이번 시험이 막 끝남 (부르는 쪽이 읽고 지움) */
    lte_off_t off_way;
    lte_result_t res;
    uint32_t tests;
    char line[LTE_LINE_MAX];
    size_t line_len;
} lte_t;

void lte_init(lte_t *l, const lte_io_t *io);
bool lte_busy(const lte_t *l);
/* 켜기 시작 (이미 하는 중이면 false). 셀 3.6 V 확인은 부르는 쪽이 함.
 * 모듈이 이미 켜져 있으면(STATUS 높음) PWRKEY 를 누르지 않고 바로 다음 단계로 감 — 켜진 모듈에 누르면 꺼짐 */
bool lte_start(lte_t *l, uint32_t now_s);
/* 지금 켜져 있나 (STATUS) */
bool lte_is_on(const lte_t *l);
/* 켜져 있는 모듈을 끄기만 함 (PWRKEY → 안 되면 리셋 뒤 PWRKEY). 꺼져 있거나 하는 중이면 false */
bool lte_off_start(lte_t *l, uint32_t now_s);
/* 1 s 마다 부름. 아직 하는 중이면 true */
bool lte_step(lte_t *l, uint32_t now_s);
/* 셀이 낮아 쉬기 전 등: 가능한 빨리 끄는 쪽으로 (끝날 때까지 lte_step 계속 부를 것) */
void lte_stop(lte_t *l);
/* LoRa 로 올릴 결과 7 바이트: 형식 1 · 결과 · 끈 방법 · 등록 상태 · 신호 · 걸린 초(큰 쪽 먼저) */
size_t lte_result_encode(const lte_result_t *r, uint8_t out[7]);
const char *lte_code_name(lte_code_t c);

/* 아래는 맥 시험용으로 드러내 둠 */
int lte_cmd(lte_t *l, const char *cmd, const char *want, char *val, size_t val_len, uint32_t timeout_ms);
