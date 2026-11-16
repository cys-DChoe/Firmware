/* 가짜 W25Q128 — 데이터시트대로 움직임 (맥 시험에서 함께 씀) */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "w25q.h"

typedef struct {
    uint8_t *mem;
    bool wel, asleep, dead;    /* dead: 전원 끊긴 뒤 — 다시 켤 때까지 아무것도 안 함 */
    uint32_t jedec;
    int busy_polls;          /* 다음 상태 읽기에서 BUSY 로 답할 횟수 */
    int busy_after_write;    /* 쓰기 · 지우기 뒤 BUSY 횟수 */
    int cut_program_at;      /* ≥0: 다음 페이지 쓰기에서 이 바이트까지만 쓰고 끊김 */
    int cut_erase_at;        /* ≥0: 다음 지우기에서 이 바이트까지만 지우고 끊김 */
    int stuck_program;       /* >0: 다음 쓰기 몇 번은 아무것도 안 써짐(불량 흉내) */
    uint32_t clock;
    int programs, erases, reads;
    uint8_t last_cmd;
} chip_t;

extern chip_t C;
extern const spi_dev_t DEV;
void chip_new(void);
