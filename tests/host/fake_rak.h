/* 가짜 RAK3172 — RUI3 매뉴얼 답 형식대로 답함 (맥 시험에서 함께 씀) */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "rak3172.h"

#define NKEY 16
typedef struct {
    char rxq[16384];
    size_t head, tail;
    uint32_t clock;
    char txline[RAK_LINE_MAX + 8];
    size_t txlen;
    /* 모듈 상태 */
    const char *keys[NKEY];
    char vals[NKEY][80];
    int nkeys;
    bool echo, asleep, dead, joined;
    uint32_t awake_until;      /* LPM 켜짐: 이 시각이 지나면 잠 */
    uint32_t mod_baud, mcu_baud, saved_baud;   /* 모듈 · MCU 속도, 저장된 값 */
    bool baud_after_reset;     /* true: AT+BAUD 는 저장만, 리셋 뒤 바뀜 */
    int baud_switches;
    int busy_left;
    const char *inject;        /* 다음 답의 값 줄과 OK 사이에 끼울 줄 */
    char log[64][RAK_LINE_MAX];   /* 받은 명령 */
    int nlog;
    int resets;
    bool rst_level;
    uint32_t rst_low_ms;
    char ltime[48];           /* AT+LTIME=? 답 (비면 "00h00m00s on 01/01/2000") */
} fake_t;

void push(fake_t *f, const char *s);
char *val_of(fake_t *f, const char *key);
void set_key(fake_t *f, const char *key, const char *v);
int fk_write(void *ctx, const char *s, size_t n);
int fk_getc(void *ctx);
uint32_t fk_ms(void *ctx);
void fk_delay(void *ctx, uint32_t ms);
void fk_rst(void *ctx, bool level);
void fk_set_baud(void *ctx, uint32_t baud);
extern fake_t F;
extern rak_t R;
void fresh(void);
int count_writes(const char *prefix);
