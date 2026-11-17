/* 맥 시험용 가짜 EG915U — 시간(초)에 따라 켜지고 · 대답하고 · 꺼짐 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lte.h"

typedef struct {
    uint32_t *now;                 /* 시험이 돌리는 초 시계 */
    uint32_t ms;
    bool on, key_down, io, rst_down;
    uint32_t key_t, boot_t, off_at;
    /* 시험에서 바꾸는 성질 */
    bool dead;                     /* 전원이 안 들어옴 */
    bool no_vddio2;
    bool sim;                      /* 유심 있음 */
    uint32_t reg_after;            /* 켜진 뒤 이 초가 지나면 등록 */
    uint8_t reg_stat;              /* 등록되면 이 값 (1 · 5) */
    uint32_t qpowd_delay;          /* AT+QPOWD 뒤 꺼질 때까지 초 (보통 3, 망 상태에 따라 길어짐) */
    uint8_t creg_stat;             /* 2G 등록 상태 (+CREG) — 0 = 안 됨 */
    bool qpowd_ignored, key_off_ignored, key_ignored_until_reset;
    int presses, resets, cmds;
    char rx[512];
    int rx_head, rx_tail;
    char tx[128];
    int tx_len;
} fake_lte_t;

void fake_lte_new(fake_lte_t *f, uint32_t *now);
void fake_lte_io(fake_lte_t *f, lte_io_t *io);
