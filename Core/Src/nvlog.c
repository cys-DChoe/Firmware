#include <string.h>
#include "nvlog.h"
#include "ring.h"      /* ring_crc16 */

#define MAGIC0 'N'
#define MAGIC1 'V'

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool all_ff(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

static bool valid(const uint8_t *b)
{
    const uint16_t len = (uint16_t)(b[2] | b[3] << 8);
    if (b[0] != MAGIC0 || b[1] != MAGIC1 || len > NVLOG_MAX_DATA) {
        return false;
    }
    const uint16_t crc = (uint16_t)(b[NVLOG_HDR + len] | b[NVLOG_HDR + len + 1] << 8);
    return crc == ring_crc16(b, NVLOG_HDR + len);
}

static uint32_t sec_addr(const nvlog_t *l, uint8_t s) { return l->base + s * l->ops->sector_size; }

nvlog_err_t nvlog_mount(nvlog_t *l, const flash_ops_t *ops, uint32_t base)
{
    if (ops == NULL || ops->sector_size < 2u * NVLOG_REC_SIZE || base % ops->sector_size != 0u) {
        return NVLOG_E_ARG;
    }
    memset(l, 0, sizeof(*l));
    l->ops = ops;
    l->base = base;
    l->slots = ops->sector_size / NVLOG_REC_SIZE;

    uint8_t b[NVLOG_REC_SIZE];
    uint32_t best_seq = 0;
    int best_sec = -1;
    uint32_t used[2] = {0, 0};   /* 섹터마다 마지막으로 쓴(FF 아닌) 칸 + 1 */
    for (uint8_t s = 0; s < 2; s++) {
        for (uint32_t k = 0; k < l->slots; k++) {
            const uint32_t a = sec_addr(l, s) + k * NVLOG_REC_SIZE;
            if (ops->read(ops->ctx, a, b, sizeof(b)) != 0) {
                return NVLOG_E_FLASH;
            }
            if (all_ff(b, sizeof(b))) {
                continue;
            }
            used[s] = k + 1u;
            if (valid(b)) {
                const uint32_t seq = rd32(b + 4);
                if (!l->has_last || seq > best_seq) {
                    l->has_last = true;
                    best_seq = seq;
                    best_sec = s;
                    l->last_addr = a;
                }
            }
        }
    }
    if (!l->has_last) {
        /* 처음: 섹터 0 부터 (쓰다 만 칸이 있으면 그 뒤부터) */
        l->cur_sec = 0;
        l->cur_slot = used[0];
        l->next_seq = 0;
        return NVLOG_OK;
    }
    l->cur_sec = (uint8_t)best_sec;
    l->cur_slot = used[best_sec];
    l->next_seq = best_seq + 1u;
    return NVLOG_OK;
}

nvlog_err_t nvlog_load(const nvlog_t *l, void *data, size_t len, size_t *got)
{
    if (!l->has_last) {
        return NVLOG_E_EMPTY;
    }
    uint8_t b[NVLOG_REC_SIZE];
    if (l->ops->read(l->ops->ctx, l->last_addr, b, sizeof(b)) != 0) {
        return NVLOG_E_FLASH;
    }
    const uint16_t n = (uint16_t)(b[2] | b[3] << 8);
    const size_t k = n < len ? n : len;
    memset(data, 0, len);              /* 옛 판(짧은 내용)에 새 칸이 생기면 0 으로 */
    memcpy(data, b + NVLOG_HDR, k);
    if (got != NULL) {
        *got = n;
    }
    return NVLOG_OK;
}

nvlog_err_t nvlog_save(nvlog_t *l, const void *data, size_t len)
{
    if (len > NVLOG_MAX_DATA) {
        return NVLOG_E_ARG;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        if (l->cur_slot >= l->slots) {
            /* 다른 섹터를 지우고 넘어감 (지금 섹터의 마지막 값은 그대로 남아 있음) */
            const uint8_t other = (uint8_t)(l->cur_sec ^ 1u);
            if (l->ops->erase(l->ops->ctx, sec_addr(l, other)) != 0) {
                return NVLOG_E_FLASH;
            }
            l->cur_sec = other;
            l->cur_slot = 0;
        }
        uint8_t b[NVLOG_REC_SIZE];
        memset(b, 0xFF, sizeof(b));
        b[0] = MAGIC0;
        b[1] = MAGIC1;
        b[2] = (uint8_t)len;
        b[3] = (uint8_t)(len >> 8);
        b[4] = (uint8_t)l->next_seq;
        b[5] = (uint8_t)(l->next_seq >> 8);
        b[6] = (uint8_t)(l->next_seq >> 16);
        b[7] = (uint8_t)(l->next_seq >> 24);
        memcpy(b + NVLOG_HDR, data, len);
        const uint16_t crc = ring_crc16(b, (uint32_t)(NVLOG_HDR + len));
        b[NVLOG_HDR + len] = (uint8_t)crc;
        b[NVLOG_HDR + len + 1] = (uint8_t)(crc >> 8);

        const uint32_t a = sec_addr(l, l->cur_sec) + l->cur_slot * NVLOG_REC_SIZE;
        l->cur_slot++;
        uint8_t chk[NVLOG_REC_SIZE];
        if (l->ops->program(l->ops->ctx, a, b, sizeof(b)) != 0 || l->ops->read(l->ops->ctx, a, chk, sizeof(chk)) != 0) {
            return NVLOG_E_FLASH;
        }
        if (memcmp(b, chk, sizeof(b)) == 0) {
            l->has_last = true;
            l->last_addr = a;
            l->next_seq++;
            return NVLOG_OK;
        }
        /* 안 써진 칸은 버리고 한 번 더 */
    }
    return NVLOG_E_VERIFY;
}
