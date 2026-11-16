/* 작은 값 저장 (누적량 · 설정 · 키) — "마지막에 쓴 것이 이김" 기록 두 섹터 번갈아 쓰기
 *
 * 한 칸 = 머리(표 2 B "NV" · 길이 2 B · 순번 4 B) + 내용 + CRC-16 2 B, 칸 크기 64 B 고정.
 * 한 섹터가 차면 다른 섹터를 지우고 그쪽에 이어 씀 → 지우는 중 전원이 끊겨도 앞 섹터에 마지막 값이 남음.
 * 켤 때 두 섹터를 훑어 순번이 가장 큰 올바른 칸을 마지막 값으로 봄.
 *
 * 플래시는 flash_ops_t 로만 만짐 → 지금은 외부 W25Q128 위 4 MB(0xC00000~)에 둠.
 * BOM 메모의 MCU 내장 고내구 데이터 플래시(EDATA)로 옮길 땐 이 ops 만 바꾸면 됨
 * (EDATA 주소 · 섹터 번호 · 지운 칸 읽을 때 ECC 오류 처리는 RM0481 확인 뒤).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NVLOG_REC_SIZE   64u
#define NVLOG_HDR        8u
#define NVLOG_MAX_DATA   (NVLOG_REC_SIZE - NVLOG_HDR - 2u)   /* 54 B */

typedef struct {
    int (*read)(void *ctx, uint32_t addr, uint8_t *buf, size_t n);
    int (*program)(void *ctx, uint32_t addr, const uint8_t *buf, size_t n);   /* 한 칸(64 B) 안에서만 부름 */
    int (*erase)(void *ctx, uint32_t addr);                                    /* 섹터 하나 */
    uint32_t sector_size;
    void *ctx;
} flash_ops_t;

typedef enum {
    NVLOG_OK = 0,
    NVLOG_E_FLASH = -1,
    NVLOG_E_EMPTY = -2,     /* 아직 저장한 적 없음 */
    NVLOG_E_ARG = -3,
    NVLOG_E_VERIFY = -4,
} nvlog_err_t;

typedef struct {
    const flash_ops_t *ops;
    uint32_t base;          /* 섹터 두 개: base, base + sector_size */
    uint32_t slots;         /* 섹터당 칸 수 */
    uint32_t next_seq;      /* 다음에 쓸 순번 */
    uint8_t cur_sec;        /* 지금 쓰는 섹터 0 · 1 */
    uint32_t cur_slot;      /* 그 섹터에서 다음에 쓸 칸 */
    bool has_last;
    uint32_t last_addr;     /* 마지막 올바른 칸 */
} nvlog_t;

nvlog_err_t nvlog_mount(nvlog_t *l, const flash_ops_t *ops, uint32_t base);
nvlog_err_t nvlog_load(const nvlog_t *l, void *data, size_t len, size_t *got);
nvlog_err_t nvlog_save(nvlog_t *l, const void *data, size_t len);
