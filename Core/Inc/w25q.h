/* 외부 NOR 플래시 W25Q128JV (16 MB) — SPI1 PA5/PA6/PA7, CS PC4
 * 근거: Winbond W25Q128JV 데이터시트 (v1_work/datasheets/w25q128jv.txt)
 *   페이지 256 B · 섹터 4 KB · JEDEC ID EF 40 18
 *   읽기 03h(≤ 50 MHz) · 쓰기 허용 06h · 상태 05h(BUSY = 비트0) · 페이지 쓰기 02h(tPP 최대 3 ms)
 *   섹터 지우기 20h(tSE 최대 400 ms) · 잠 B9h · 깨우기 ABh(tRES1 3 µs)
 *   쓰기는 지운(FFh) 자리에만, 비트는 1 → 0 으로만 바뀜. 페이지 끝을 넘으면 앞으로 감아 덮음.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define W25Q_PAGE      256u
#define W25Q_SECTOR    4096u
#define W25Q_SIZE      (16u * 1024u * 1024u)
#define W25Q_JEDEC     0xEF4018u

typedef enum {
    W25Q_OK = 0,
    W25Q_E_ID = -1,        /* 다른 칩 · 응답 없음 */
    W25Q_E_TIMEOUT = -2,   /* BUSY 가 안 풀림 */
    W25Q_E_ARG = -3,       /* 주소 · 길이 잘못(페이지 넘김 등) */
    W25Q_E_BUS = -4,
} w25q_err_t;

/* SPI 한 번 주고받기: CS 낮춤 → tx n1 바이트 보냄 → rx n2 바이트 받음 → CS 올림 */
typedef struct {
    int (*xfer)(void *ctx, const uint8_t *tx, size_t ntx, uint8_t *rx, size_t nrx);
    /* 명령 + 데이터 쓰기 (페이지 쓰기): CS 낮춤 → a 보냄 → b 보냄 → CS 올림 */
    int (*xfer2)(void *ctx, const uint8_t *a, size_t na, const uint8_t *b, size_t nb);
    uint32_t (*ms)(void *ctx);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} spi_dev_t;

w25q_err_t w25q_init(const spi_dev_t *d);          /* 깨우기 → JEDEC ID 확인 */
w25q_err_t w25q_read(const spi_dev_t *d, uint32_t addr, uint8_t *buf, size_t n);
w25q_err_t w25q_program(const spi_dev_t *d, uint32_t addr, const uint8_t *buf, size_t n);  /* 한 페이지 안에서만 */
w25q_err_t w25q_erase_sector(const spi_dev_t *d, uint32_t addr);
w25q_err_t w25q_sleep(const spi_dev_t *d);          /* 깊은 잠 (약 1 µA) */
