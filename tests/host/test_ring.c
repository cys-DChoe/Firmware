/* 외부 플래시(W25Q128) · 1분 기록 링 맥 시험
 * 가짜 칩은 데이터시트대로 움직임: 쓰기 허용(WEL) 없으면 무시 · 비트는 1 → 0 만 · 페이지 끝에서 앞으로 감음 ·
 * 지우기 = FFh · 잠(B9h) 중엔 ABh 만 받음 · BUSY 를 몇 번 보여 줌. 전원 끊김은 "몇 바이트만 쓰고 멈춤"으로 흉내냄.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ring.h"
#include "w25q.h"

static int fails = 0, checks = 0;

static void ok(const char *what, bool v)
{
    checks++;
    if (!v) {
        fails++;
    }
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

/* ---------- 가짜 W25Q128 ---------- */

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

static chip_t C;

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

static const spi_dev_t DEV = {ch_xfer, ch_xfer2, ch_ms, ch_delay, &C};

static void chip_new(void)
{
    free(C.mem);
    memset(&C, 0, sizeof(C));
    C.mem = malloc(W25Q_SIZE);
    memset(C.mem, 0xFF, W25Q_SIZE);
    C.jedec = W25Q_JEDEC;
    C.cut_program_at = -1;
    C.cut_erase_at = -1;
}

static void rep_for(uint32_t i, uint8_t *rep)
{
    for (uint32_t k = 0; k < RING_REPORT_SIZE; k++) {
        rep[k] = (uint8_t)(i * 7u + k);
    }
}

static bool read_matches(const ring_t *r, uint32_t seq, uint32_t i)
{
    ring_rec_t rec;
    uint8_t rep[RING_REPORT_SIZE];
    rep_for(i, rep);
    return ring_read(r, seq, &rec) == RING_OK && rec.seq == seq && memcmp(rec.report, rep, sizeof(rep)) == 0;
}

/* ---------- 시험 ---------- */

static void test_w25q(void)
{
    printf("W25Q128 명령 (가짜 칩)\n");
    chip_new();
    ok("JEDEC EF 40 18 → 시작 OK", w25q_init(&DEV) == W25Q_OK);
    C.jedec = 0xC22018;   /* 다른 회사 칩 */
    ok("다른 칩 ID → 오류", w25q_init(&DEV) == W25Q_E_ID);
    C.jedec = W25Q_JEDEC;
    ok("잠 → 시작하면 ABh 로 깨워서 OK", w25q_sleep(&DEV) == W25Q_OK && C.asleep && w25q_init(&DEV) == W25Q_OK && !C.asleep);

    const uint8_t d[4] = {0x12, 0x34, 0x56, 0x78};
    uint8_t got[4];
    C.busy_after_write = 3;
    ok("페이지 쓰기 → 다시 읽기 같음 (BUSY 3번 기다림)",
       w25q_program(&DEV, 0x1000, d, 4) == W25Q_OK && w25q_read(&DEV, 0x1000, got, 4) == W25Q_OK && memcmp(d, got, 4) == 0);
    const uint8_t z[1] = {0xF0};
    w25q_program(&DEV, 0x1000, z, 1);
    w25q_read(&DEV, 0x1000, got, 1);
    ok("이미 쓴 바이트에 또 쓰면 1 → 0 만 (12 & F0 = 10)", got[0] == 0x10);
    ok("페이지 끝 넘는 쓰기는 막음", w25q_program(&DEV, 0x10FE, d, 4) == W25Q_E_ARG);
    ok("섹터 지우기 → FFh", w25q_erase_sector(&DEV, 0x1000) == W25Q_OK && w25q_read(&DEV, 0x1000, got, 4) == W25Q_OK &&
                               got[0] == 0xFF && got[3] == 0xFF);
    ok("섹터 경계 아닌 지우기는 막음", w25q_erase_sector(&DEV, 0x1100) == W25Q_E_ARG);
    ok("칩 끝 넘는 읽기는 막음", w25q_read(&DEV, W25Q_SIZE - 2, got, 4) == W25Q_E_ARG);
    C.busy_after_write = 100000;
    ok("BUSY 가 안 풀리면 시간 초과", w25q_program(&DEV, 0x2000, d, 4) == W25Q_E_TIMEOUT);
    C.busy_polls = 0;
    C.busy_after_write = 0;
}

static void test_ring_basic(void)
{
    printf("링 — 처음 · 쓰기 · 다시 켜기\n");
    chip_new();
    ring_t r;
    ok("빈 칩 → 순번 0 부터", ring_mount(&r, &DEV, 0, RING_DEFAULT_SECTORS) == RING_OK && r.next_seq == 0);
    ok("기본 칸 수 393,216 ≥ 90일(129,600)", r.slots == 393216u && r.slots >= RING_90_DAYS);
    uint8_t rep[RING_REPORT_SIZE];
    uint32_t seq = 99;
    for (uint32_t i = 0; i < 3; i++) {
        rep_for(i, rep);
        ring_append(&r, rep, &seq);
    }
    ok("세 개 쓰면 순번 0 · 1 · 2", seq == 2 && r.next_seq == 3);
    ok("읽은 값 = 쓴 값", read_matches(&r, 0, 0) && read_matches(&r, 2, 2));
    ok("아직 안 쓴 순번 3 은 없음", !read_matches(&r, 3, 3));
    ok("첫 칸 쓸 때 섹터 한 번 지움", C.erases == 1);
    const uint8_t *raw = C.mem + 32;
    ok("칸 배치: 순번(작은 쪽 먼저) · 보고 24 B · 표시 FFh · 예비 FFh · CRC",
       raw[0] == 1 && raw[1] == 0 && raw[4] == 7 && raw[28] == 0xFF && raw[29] == 0xFF &&
           (raw[30] | raw[31] << 8) == ring_crc16(raw, 28));

    ring_t r2;
    ok("다시 켜면 다음 순번 3", ring_mount(&r2, &DEV, 0, RING_DEFAULT_SECTORS) == RING_OK && r2.next_seq == 3);
    ring_rec_t last;
    ok("마지막 기록 = 순번 2 (누적량 되살리기)", ring_last(&r2, &last) == RING_OK && last.seq == 2);
    ok("CRC-16/CCITT-FALSE \"123456789\" = 29B1", ring_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
}

static void test_ring_wrap(void)
{
    printf("링 — 한 바퀴 넘기기 (4섹터 = 512칸으로 줄여 시험)\n");
    chip_new();
    ring_t r;
    ring_mount(&r, &DEV, 0x100000, 4);
    uint8_t rep[RING_REPORT_SIZE];
    for (uint32_t i = 0; i < 600; i++) {
        rep_for(i, rep);
        ring_append(&r, rep, NULL);
    }
    ok("600개 쓰면 다음 순번 600", r.next_seq == 600);
    ok("가장 오래 남은 순번 128 (0–127 섹터는 지워짐)", r.oldest_seq == 128);
    ok("순번 127 없음 · 128 있음 · 599 있음", !read_matches(&r, 127, 127) && read_matches(&r, 128, 128) && read_matches(&r, 599, 599));
    ok("섹터 지우기 5번 (512칸 + 1섹터)", C.erases == 5);
    ok("링 밖(0x100000 앞) 은 안 건드림", C.mem[0x0FFFFF] == 0xFF && C.mem[0x104000] == 0xFF);
    ring_t r2;
    ring_mount(&r2, &DEV, 0x100000, 4);
    ok("다시 켜도 600 · 128", r2.next_seq == 600 && r2.oldest_seq == 128);
}

static void test_ring_power_cut(void)
{
    printf("링 — 쓰다가 전원 끊김\n");
    chip_new();
    ring_t r;
    ring_mount(&r, &DEV, 0, 4);
    uint8_t rep[RING_REPORT_SIZE];
    for (uint32_t i = 0; i < 300; i++) {
        rep_for(i, rep);
        ring_append(&r, rep, NULL);
    }
    C.cut_program_at = 10;                    /* 순번 300 을 10바이트만 쓰고 끊김 */
    rep_for(300, rep);
    ring_append(&r, rep, NULL);
    C.dead = false;
    ring_t r2;
    ring_mount(&r2, &DEV, 0, 4);
    ok("다시 켜면 깨진 칸 건너 다음 순번 301", r2.next_seq == 301);
    ok("깨진 300 은 없음 · 299 는 있음", !read_matches(&r2, 300, 300) && read_matches(&r2, 299, 299));
    ring_rec_t last;
    ok("마지막 기록은 깨진 칸 앞 299", ring_last(&r2, &last) == RING_OK && last.seq == 299);
    rep_for(301, rep);
    uint32_t seq;
    ok("이어서 쓰면 301", ring_append(&r2, rep, &seq) == RING_OK && seq == 301 && read_matches(&r2, 301, 301));

    printf("링 — 새 섹터 첫 칸에서 끊김\n");
    for (uint32_t i = 302; i < 384; i++) {
        rep_for(i, rep);
        ring_append(&r2, rep, NULL);
    }
    C.cut_program_at = 5;                     /* 순번 384 = 섹터 3 첫 칸 */
    rep_for(384, rep);
    ring_append(&r2, rep, NULL);
    C.dead = false;
    ring_t r3;
    ring_mount(&r3, &DEV, 0, 4);
    ok("깨진 첫 칸 섹터는 무시 → 다음 순번 384 (다시 지우고 씀)", r3.next_seq == 384);
    const int erases = C.erases;
    rep_for(384, rep);
    ok("384 다시 씀 · 섹터 다시 지움", ring_append(&r3, rep, &seq) == RING_OK && seq == 384 && C.erases == erases + 1 &&
                                             read_matches(&r3, 384, 384));

    printf("링 — 섹터 지우다 끊김 (반만 지워짐)\n");
    for (uint32_t i = 385; i < 512; i++) {
        rep_for(i, rep);
        ring_append(&r3, rep, NULL);
    }
    C.cut_erase_at = 2048;                    /* 순번 512 = 섹터 0 두 바퀴째, 반만 지워짐 */
    rep_for(512, rep);
    ring_append(&r3, rep, NULL);              /* 지우는 중 끊김 → 쓰기는 못 함 */
    C.dead = false;
    ring_t r4;
    ring_mount(&r4, &DEV, 0, 4);
    ok("다시 켜면 이어 쓸 순번 512", r4.next_seq == 512);
    ok("지워진 앞 반(순번 10)은 없음 · 남은 뒤 반(순번 100)은 옛 값 그대로",
       !read_matches(&r4, 10, 10) && read_matches(&r4, 100, 100));
    rep_for(512, rep);
    ok("512 를 쓰면 섹터를 처음부터 다시 다 지움", ring_append(&r4, rep, &seq) == RING_OK && seq == 512 &&
                                                         read_matches(&r4, 512, 512) && !read_matches(&r4, 100, 100));
    ring_t r5;
    ring_mount(&r5, &DEV, 0, 4);
    ok("다시 켜면 513", r5.next_seq == 513);
}

static void test_ring_sent(void)
{
    printf("링 — 보냄 표시 · 못 보낸 것 찾기\n");
    chip_new();
    ring_t r;
    ring_mount(&r, &DEV, 0, 4);
    uint8_t rep[RING_REPORT_SIZE];
    for (uint32_t i = 0; i < 20; i++) {
        rep_for(i, rep);
        ring_append(&r, rep, NULL);
    }
    for (uint32_t s = 0; s < 20; s++) {
        if (s != 12 && s != 15) {
            ring_mark_sent(&r, s);
        }
    }
    uint32_t seq = 0;
    ok("최근 10칸에서 못 보낸 가장 오래된 것 = 12", ring_find_unsent(&r, 10, &seq) == RING_OK && seq == 12);
    ok("최근 6칸에서는 15", ring_find_unsent(&r, 6, &seq) == RING_OK && seq == 15);
    ok("최근 4칸엔 없음", ring_find_unsent(&r, 4, &seq) == RING_E_MISSING);
    ring_rec_t rec;
    ok("표시를 고쳐도 보고 내용 · CRC 그대로", ring_read(&r, 11, &rec) == RING_OK && rec.sent && read_matches(&r, 11, 11));
    ok("두 번 표시해도 OK", ring_mark_sent(&r, 11) == RING_OK);
    const uint8_t *raw = C.mem + 11 * 32;
    ok("표시 바이트 FEh · 예비 FFh", raw[28] == 0xFE && raw[29] == 0xFF);
    ring_t r2;
    ring_mount(&r2, &DEV, 0, 4);
    ok("다시 켜도 12 가 못 보낸 것으로 남음", ring_find_unsent(&r2, 100, &seq) == RING_OK && seq == 12);
    ok("없는 순번 표시 → 없음", ring_mark_sent(&r2, 25) == RING_E_MISSING);
}

static void test_ring_verify(void)
{
    printf("링 — 써도 안 써지는 칸(불량)\n");
    chip_new();
    ring_t r;
    ring_mount(&r, &DEV, 0, 4);
    uint8_t rep[RING_REPORT_SIZE];
    rep_for(0, rep);
    ring_append(&r, rep, NULL);
    C.stuck_program = 1;
    rep_for(1, rep);
    uint32_t seq = 0;
    ok("한 칸 버리고 다음 칸(순번 2)에 씀", ring_append(&r, rep, &seq) == RING_OK && seq == 2 && r.skipped == 1);
    ok("순번 1 은 없음 · 2 는 그 내용", !read_matches(&r, 1, 1) && read_matches(&r, 2, 1));
    C.stuck_program = 2;
    ok("두 번 다 안 써지면 확인 실패", ring_append(&r, rep, &seq) == RING_E_VERIFY);
    ok("섹터 경계 아닌 시작 주소 거부", ring_mount(&r, &DEV, 100, 4) == RING_E_ARG);
}

int main(void)
{
    test_w25q();
    test_ring_basic();
    test_ring_wrap();
    test_ring_power_cut();
    test_ring_sent();
    test_ring_verify();
    free(C.mem);
    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
