/* 1분 기록 링 저장 — 외부 NOR 플래시(W25Q128)에 같은 길이(32 B)로 차례로 씀, 가득 차면 가장 오래된 섹터부터 지움
 *
 * 기록 한 칸 32 B (섹터 4 KB 에 128칸, 페이지 256 B 에 8칸 — 칸이 페이지를 넘지 않음):
 *   0–3   순번 seq (작은 쪽 먼저). 칸 번호와 같음: 주소 = 시작 + (seq mod 칸 수) × 32
 *   4–27  1분 보고 24 B (형식 v1, report.h) — 시각 · 누적 · 유량 · 압력 · 온도 · 배터리 · 상태가 다 들어 있음
 *   28    표시: 비트0 1 = 아직 안 보냄, 0 = 보냄 (지우지 않고 1 → 0 으로만 고침)
 *   29    예비 FFh
 *   30–31 CRC-16/CCITT-FALSE (0–27, 작은 쪽 먼저)
 *
 * 전원이 끊겨도: 쓰다 만 칸은 CRC 가 안 맞아 "없는 기록"으로 보고 건너뜀. 지우다 만 섹터는 다음 쓰기 때 다시 지움.
 * 켤 때 ring_mount() 가 각 섹터 첫 기록만 읽어 마지막 위치를 찾음(3072섹터 × 32 B).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "w25q.h"

#define RING_REC_SIZE      32u
#define RING_REPORT_SIZE   24u
#define RING_PER_SECTOR    (W25Q_SECTOR / RING_REC_SIZE)   /* 128 */
#define RING_DEFAULT_BASE  0u
#define RING_DEFAULT_SECTORS 3072u    /* 12 MB = 393,216칸 = 273일. 위 4 MB 는 나중 용도(펌웨어 받기 등)로 비워 둠 */
#define RING_90_DAYS       (90u * 1440u)

typedef enum {
    RING_OK = 0,
    RING_E_FLASH = -1,     /* 플래시 명령 실패 */
    RING_E_MISSING = -2,   /* 그 순번 기록이 없음(지워짐 · 쓰다 끊김 · 아직 안 씀) */
    RING_E_ARG = -3,
    RING_E_VERIFY = -4,    /* 두 번 써도 다시 읽은 값이 다름 */
} ring_err_t;

typedef struct {
    uint32_t seq;
    uint8_t report[RING_REPORT_SIZE];
    bool sent;
} ring_rec_t;

typedef struct {
    const spi_dev_t *dev;
    uint32_t base;          /* 섹터 경계 */
    uint32_t sectors;
    uint32_t slots;         /* sectors × 128 */
    uint32_t next_seq;      /* 다음에 쓸 순번 */
    uint32_t oldest_seq;    /* 남아 있을 수 있는 가장 오래된 순번 */
    uint32_t skipped;       /* 쓰다 끊기거나 확인 실패로 버린 칸 수(켠 뒤) */
} ring_t;

ring_err_t ring_mount(ring_t *r, const spi_dev_t *dev, uint32_t base, uint32_t sectors);
ring_err_t ring_append(ring_t *r, const uint8_t report[RING_REPORT_SIZE], uint32_t *seq_out);
ring_err_t ring_read(const ring_t *r, uint32_t seq, ring_rec_t *out);
ring_err_t ring_last(const ring_t *r, ring_rec_t *out);        /* 켠 뒤 누적량 되살리기용 */
ring_err_t ring_mark_sent(const ring_t *r, uint32_t seq);
/* 최근 max_back 칸 안에서 아직 안 보낸 가장 오래된 기록의 순번 */
ring_err_t ring_find_unsent(const ring_t *r, uint32_t max_back, uint32_t *seq_out);

/* 맥 시험용 */
uint16_t ring_crc16(const uint8_t *p, uint32_t n);
