#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_w25q.h"

/* ---------- 가짜 W25Q128 ---------- */


chip_t C;

static uint32_t a24(const uint8_t *t) { return (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3]; }

static int ch_xfer2(void *ctx, const uint8_t *a, size_t na, const uint8_t *b, size_t nb)
{
    chip_t *c = ctx;
    c->last_cmd = a[0];
    if (c->dead) {
        return 0;
    }
    if (c->asleep) {
        if (a[0] == 0xAB) {
            c->asleep = false;
        }
        return 0;
    }
    switch (a[0]) {
    case 0x06:
        c->wel = true;
        break;
    case 0xB9:
        c->asleep = true;
        break;
    case 0x02: {
        if (!c->wel || na < 4) {
            break;
        }
        c->wel = false;
        c->programs++;
        if (c->stuck_program > 0) {
            c->stuck_program--;
            break;
        }
        const uint32_t addr = a24(a);
        const uint32_t page = addr & ~0xFFu;
        size_t n = nb;
        if (c->cut_program_at >= 0 && (size_t)c->cut_program_at < n) {
            n = (size_t)c->cut_program_at;
            c->cut_program_at = -1;
            c->dead = true;
        }
        for (size_t i = 0; i < n; i++) {
            const uint32_t at = page | ((addr + i) & 0xFFu);   /* 페이지 안에서 감음 */
            c->mem[at] &= b[i];                                  /* 1 → 0 만 */
        }
        c->busy_polls = c->busy_after_write;
        break;
    }
    case 0x20: {
        if (!c->wel || na < 4) {
            break;
        }
        c->wel = false;
        c->erases++;
        const uint32_t addr = a24(a) & ~0xFFFu;
        size_t n = W25Q_SECTOR;
        if (c->cut_erase_at >= 0) {
            n = (size_t)c->cut_erase_at;
            c->cut_erase_at = -1;
            c->dead = true;
        }
        memset(c->mem + addr, 0xFF, n);
        c->busy_polls = c->busy_after_write;
        break;
    }
    default:
        break;
    }
    return 0;
}

static int ch_xfer(void *ctx, const uint8_t *tx, size_t ntx, uint8_t *rx, size_t nrx)
{
    chip_t *c = ctx;
    if (c->asleep || (tx[0] != 0x03 && tx[0] != 0x05 && tx[0] != 0x9F)) {
        if (rx != NULL) {
            memset(rx, 0xFF, nrx);
        }
        return ch_xfer2(ctx, tx, ntx, NULL, 0);
    }
    c->last_cmd = tx[0];
    if (tx[0] == 0x9F) {
        rx[0] = (uint8_t)(c->jedec >> 16);
        rx[1] = (uint8_t)(c->jedec >> 8);
        rx[2] = (uint8_t)c->jedec;
    } else if (tx[0] == 0x05) {
        rx[0] = (uint8_t)((c->busy_polls > 0 ? 0x01 : 0x00) | (c->wel ? 0x02 : 0x00));
        if (c->busy_polls > 0) {
            c->busy_polls--;
        }
    } else {
        c->reads++;
        const uint32_t addr = a24(tx);
        for (size_t i = 0; i < nrx; i++) {
            rx[i] = c->mem[(addr + i) % W25Q_SIZE];
        }
    }
    return 0;
}

static uint32_t ch_ms(void *ctx) { return ((chip_t *)ctx)->clock++; }
static void ch_delay(void *ctx, uint32_t ms) { ((chip_t *)ctx)->clock += ms; }

const spi_dev_t DEV = {ch_xfer, ch_xfer2, ch_ms, ch_delay, &C};

void chip_new(void)
{
    free(C.mem);
    memset(&C, 0, sizeof(C));
    C.mem = malloc(W25Q_SIZE);
    memset(C.mem, 0xFF, W25Q_SIZE);
    C.jedec = W25Q_JEDEC;
    C.cut_program_at = -1;
    C.cut_erase_at = -1;
}

