#include <string.h>
#include "ring.h"

#define OFF_SEQ    0u
#define OFF_REP    4u
#define OFF_FLAG   28u
#define OFF_CRC    30u
#define FLAG_UNSENT 0x01u

uint16_t ring_crc16(const uint8_t *p, uint32_t n)
{
    uint16_t c = 0xFFFFu;          /* CRC-16/CCITT-FALSE: 다항식 1021, 시작 FFFF */
    for (uint32_t i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) {
            c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
        }
    }
    return c;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t slot_addr(const ring_t *r, uint32_t seq) { return r->base + (seq % r->slots) * RING_REC_SIZE; }

static bool all_ff(const uint8_t *b)
{
    for (uint32_t i = 0; i < RING_REC_SIZE; i++) {
        if (b[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

/* 칸 하나가 올바른 기록인지: CRC · 칸 번호와 순번이 맞는지 */
static bool valid_at(const ring_t *r, const uint8_t *b, uint32_t slot)
{
    const uint16_t crc = (uint16_t)(b[OFF_CRC] | b[OFF_CRC + 1] << 8);
    return crc == ring_crc16(b, OFF_FLAG) && rd32(b + OFF_SEQ) % r->slots == slot;
}

static void set_oldest(ring_t *r)
{
    /* 쓰고 있는 섹터 바로 다음 섹터(한 바퀴 전 기록)부터 남아 있음 */
    if (r->next_seq == 0u) {
        r->oldest_seq = 0;
        return;
    }
    const uint32_t head_base = ((r->next_seq - 1u) / RING_PER_SECTOR) * RING_PER_SECTOR;
    r->oldest_seq = (head_base + RING_PER_SECTOR > r->slots) ? head_base + RING_PER_SECTOR - r->slots : 0u;
}

ring_err_t ring_mount(ring_t *r, const spi_dev_t *dev, uint32_t base, uint32_t sectors)
{
    if (base % W25Q_SECTOR != 0u || sectors < 2u || base + sectors * W25Q_SECTOR > W25Q_SIZE) {
        return RING_E_ARG;
    }
    memset(r, 0, sizeof(*r));
    r->dev = dev;
    r->base = base;
    r->sectors = sectors;
    r->slots = sectors * RING_PER_SECTOR;

    /* 1) 섹터마다 첫 올바른 기록으로 그 섹터 시작 순번을 구해 가장 큰 섹터(= 쓰는 중인 섹터)를 찾음 */
    bool found = false;
    uint32_t head_sec = 0, head_base = 0;
    uint8_t b[RING_REC_SIZE];
    for (uint32_t s = 0; s < sectors; s++) {
        for (uint32_t k = 0; k < RING_PER_SECTOR; k++) {
            const uint32_t slot = s * RING_PER_SECTOR + k;
            if (w25q_read(dev, base + slot * RING_REC_SIZE, b, sizeof(b)) != W25Q_OK) {
                return RING_E_FLASH;
            }
            if (all_ff(b)) {
                break;                      /* 이 섹터는 여기부터 비어 있음 */
            }
            if (valid_at(r, b, slot)) {
                const uint32_t sb = rd32(b + OFF_SEQ) - k;
                if (!found || sb > head_base) {
                    found = true;
                    head_sec = s;
                    head_base = sb;
                }
                break;
            }
            /* 깨진 칸(쓰다 끊김) — 다음 칸을 봄 */
        }
    }
    if (!found) {
        r->next_seq = 0;
        set_oldest(r);
        return RING_OK;
    }

    /* 2) 그 섹터에서 마지막으로 쓴(FFh 아닌) 칸 뒤가 다음 자리 */
    uint32_t last = 0;
    for (uint32_t k = 0; k < RING_PER_SECTOR; k++) {
        if (w25q_read(dev, base + (head_sec * RING_PER_SECTOR + k) * RING_REC_SIZE, b, sizeof(b)) != W25Q_OK) {
            return RING_E_FLASH;
        }
        if (!all_ff(b)) {
            last = k;
        }
    }
    r->next_seq = head_base + last + 1u;
    set_oldest(r);
    return RING_OK;
}

static void build(uint8_t *b, uint32_t seq, const uint8_t *rep)
{
    b[0] = (uint8_t)seq;
    b[1] = (uint8_t)(seq >> 8);
    b[2] = (uint8_t)(seq >> 16);
    b[3] = (uint8_t)(seq >> 24);
    memcpy(b + OFF_REP, rep, RING_REPORT_SIZE);
    b[OFF_FLAG] = 0xFFu;                 /* 안 보냄 */
    b[29] = 0xFFu;
    const uint16_t crc = ring_crc16(b, OFF_FLAG);
    b[OFF_CRC] = (uint8_t)crc;
    b[OFF_CRC + 1] = (uint8_t)(crc >> 8);
}

ring_err_t ring_append(ring_t *r, const uint8_t report[RING_REPORT_SIZE], uint32_t *seq_out)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        const uint32_t seq = r->next_seq;
        const uint32_t addr = slot_addr(r, seq);
        if (seq % RING_PER_SECTOR == 0u) {
            /* 새 섹터 첫 칸: 한 바퀴 전 기록 128개를 지움 */
            if (w25q_erase_sector(r->dev, addr) != W25Q_OK) {
                return RING_E_FLASH;
            }
        }
        r->next_seq = seq + 1u;
        set_oldest(r);

        uint8_t b[RING_REC_SIZE], chk[RING_REC_SIZE];
        build(b, seq, report);
        if (w25q_program(r->dev, addr, b, sizeof(b)) != W25Q_OK ||
            w25q_read(r->dev, addr, chk, sizeof(chk)) != W25Q_OK) {
            return RING_E_FLASH;
        }
        if (memcmp(b, chk, sizeof(b)) == 0) {
            if (seq_out != NULL) {
                *seq_out = seq;
            }
            return RING_OK;
        }
        r->skipped++;                       /* 이 칸은 버리고 다음 칸에 한 번 더 */
    }
    return RING_E_VERIFY;
}

static bool in_window(const ring_t *r, uint32_t seq)
{
    return seq < r->next_seq && seq >= r->oldest_seq;
}

ring_err_t ring_read(const ring_t *r, uint32_t seq, ring_rec_t *out)
{
    if (!in_window(r, seq)) {
        return RING_E_MISSING;
    }
    uint8_t b[RING_REC_SIZE];
    if (w25q_read(r->dev, slot_addr(r, seq), b, sizeof(b)) != W25Q_OK) {
        return RING_E_FLASH;
    }
    if (!valid_at(r, b, seq % r->slots) || rd32(b + OFF_SEQ) != seq) {
        return RING_E_MISSING;
    }
    out->seq = seq;
    memcpy(out->report, b + OFF_REP, RING_REPORT_SIZE);
    out->sent = (b[OFF_FLAG] & FLAG_UNSENT) == 0u;
    return RING_OK;
}

ring_err_t ring_last(const ring_t *r, ring_rec_t *out)
{
    /* 마지막 칸이 깨졌으면 한 섹터 안에서 거슬러 찾음 */
    for (uint32_t i = 1; i <= RING_PER_SECTOR && i <= r->next_seq; i++) {
        const ring_err_t e = ring_read(r, r->next_seq - i, out);
        if (e != RING_E_MISSING) {
            return e;
        }
    }
    return RING_E_MISSING;
}

ring_err_t ring_mark_sent(const ring_t *r, uint32_t seq)
{
    ring_rec_t rec;
    const ring_err_t e = ring_read(r, seq, &rec);
    if (e != RING_OK) {
        return e;
    }
    if (rec.sent) {
        return RING_OK;
    }
    const uint8_t v = (uint8_t)~FLAG_UNSENT;   /* FFh → FEh: 비트 하나만 1 → 0 */
    return w25q_program(r->dev, slot_addr(r, seq) + OFF_FLAG, &v, 1) == W25Q_OK ? RING_OK : RING_E_FLASH;
}

ring_err_t ring_find_unsent(const ring_t *r, uint32_t max_back, uint32_t *seq_out)
{
    bool found = false;
    for (uint32_t i = 1; i <= max_back && i <= r->next_seq; i++) {
        const uint32_t seq = r->next_seq - i;
        if (seq < r->oldest_seq) {
            break;
        }
        ring_rec_t rec;
        const ring_err_t e = ring_read(r, seq, &rec);
        if (e == RING_E_FLASH) {
            return e;
        }
        if (e == RING_OK && !rec.sent) {
            *seq_out = seq;                 /* 거슬러 가며 덮어써서 가장 오래된 것이 남음 */
            found = true;
        }
    }
    return found ? RING_OK : RING_E_MISSING;
}
