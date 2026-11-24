/* 누적 · 설정 · 키 저장 (nvlog · persist) 맥 시험 — 가짜 W25Q128 위에서, 전원 끊김 포함 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fake_w25q.h"
#include "nv_w25q.h"
#include "persist.h"
#include "ring.h"

static int fails = 0, checks = 0;

static void ok(const char *what, bool v)
{
    checks++;
    if (!v) {
        fails++;
    }
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

static flash_ops_t OPS;

static counter_t ctr(uint32_t i)
{
    counter_t c = {0};
    c.total_nm3 = 12.345678901 + i * 0.001;
    c.total_m3 = 13.5 + i * 0.0011;
    c.ring_next_seq = 1000u + i;
    c.report_seq = (uint16_t)(65530u + i);   /* 65535 을 넘으면 0 부터 */
    c.vent_minutes = 7u + i;
    c.depletions = 2;
    c.events = 30u + i;
    c.unix_s = 1794819600u + 60u * i;
    return c;
}

static bool same_ctr(const counter_t *a, const counter_t *b)
{
    return a->total_nm3 == b->total_nm3 && a->total_m3 == b->total_m3 && a->ring_next_seq == b->ring_next_seq &&
           a->report_seq == b->report_seq && a->vent_minutes == b->vent_minutes && a->depletions == b->depletions &&
           a->events == b->events && a->unix_s == b->unix_s;
}

/* ---------- 가짜 내장 EDATA: 섹터 6 KB, 16비트 단위, 안 쓴 칸 읽으면 ECC 오류(NMI) 수를 셈, 덮어쓰기 감시 ---------- */
#define ED_SIZE (4u * 0x1800u)
static uint16_t ed_mem[ED_SIZE / 2];
static bool ed_written[ED_SIZE / 2];
static int ed_virgin_reads, ed_double_programs, ed_cut_after = -1;

static int ed_rd(void *ctx, uint32_t a, uint8_t *b, size_t n)
{
    (void)ctx;
    if ((a | n) & 1u) return -1;
    for (size_t i = 0; i < n; i += 2) {
        const uint32_t k = (a + (uint32_t)i) / 2u;
        uint16_t v = ed_mem[k];
        if (!ed_written[k]) {
            ed_virgin_reads++;          /* 보드에선 NMI 한 번 */
            v = 0xFFFF;
        }
        b[i] = (uint8_t)v;
        b[i + 1] = (uint8_t)(v >> 8);
    }
    return 0;
}

static int ed_pg(void *ctx, uint32_t a, const uint8_t *b, size_t n)
{
    (void)ctx;
    if ((a | n) & 1u) return -1;
    for (size_t i = 0; i < n; i += 2) {
        if (ed_cut_after == 0) {
            return 0;                   /* 전원 끊김: 여기까지만 써짐 */
        }
        if (ed_cut_after > 0) ed_cut_after--;
        const uint32_t k = (a + (uint32_t)i) / 2u;
        if (ed_written[k]) ed_double_programs++;
        ed_mem[k] = (uint16_t)(b[i] | b[i + 1] << 8);
        ed_written[k] = true;
    }
    return 0;
}

static int ed_er(void *ctx, uint32_t a)
{
    (void)ctx;
    if (a % 0x1800u) return -1;
    for (uint32_t k = a / 2u; k < (a + 0x1800u) / 2u; k++) {
        ed_mem[k] = 0xFFFF;
        ed_written[k] = false;
    }
    return 0;
}

static void test_edata(void)
{
    printf("내장 EDATA 흉내 (섹터 6 KB · 96칸)\n");
    static flash_ops_t E = {ed_rd, ed_pg, ed_er, 0x1800u, NULL};
    nvlog_t s1, c1;
    ed_virgin_reads = 0;
    ok("빈 EDATA 열기", nvlog_mount(&s1, &E, 0) == NVLOG_OK && nvlog_mount(&c1, &E, 0x3000) == NVLOG_OK && s1.slots == 96);
    printf("       (빈 칸 4섹터 열 때 ECC 오류 %d 번)\n", ed_virgin_reads);
    ok("빈 칸은 머리 8 B(16비트 4개)만 읽음 → 384칸 × 4 = 1536 번", ed_virgin_reads == 4 * 96 * 4);
    settings_t w, r;
    settings_default(&w);
    w.zero_pa = 1.25;
    w.zero_set = true;
    ok("설정 저장 · 읽기", settings_save(&s1, &w) == NVLOG_OK && settings_load(&s1, &r) == NVLOG_OK && r.zero_pa == 1.25);
    counter_t x, c;
    for (uint32_t i = 0; i < 300; i++) {
        x = ctr(i);
        counter_save(&c1, &x);
    }
    ok("계수 300번 (섹터 3번 넘김) 뒤 마지막 값", counter_load(&c1, &c) == NVLOG_OK && same_ctr(&c, &x));
    ok("16비트 칸을 지우지 않고 두 번 쓴 적 없음", ed_double_programs == 0);
    nvlog_t c2;
    ed_virgin_reads = 0;
    nvlog_mount(&c2, &E, 0x3000);
    ok("다시 켜도 마지막 값", counter_load(&c2, &c) == NVLOG_OK && same_ctr(&c, &x));
    ok("쓴 칸은 ECC 오류 없이 읽힘 (빈 칸 머리만 오류)", ed_virgin_reads <= 2 * 96 * 4);
    ed_cut_after = 10;                  /* 64 B 칸 중 16비트 10개만 쓰고 끊김 */
    counter_t y = ctr(999);
    counter_save(&c2, &y);
    ed_cut_after = -1;
    nvlog_t c3;
    nvlog_mount(&c3, &E, 0x3000);
    ok("쓰다 끊긴 칸은 버리고 앞 값", counter_load(&c3, &c) == NVLOG_OK && same_ctr(&c, &x));
    ok("이어서 저장 (덮어쓰기 없이 다음 칸)", counter_save(&c3, &y) == NVLOG_OK && counter_load(&c3, &c) == NVLOG_OK &&
                                                 same_ctr(&c, &y) && ed_double_programs == 0);
    ok("설정 자리는 그대로", settings_load(&s1, &r) == NVLOG_OK && r.zero_pa == 1.25);
}

int main(void)
{
    chip_new();
    nv_w25q_ops(&OPS, &DEV);

    printf("처음 (아무것도 저장 안 됨)\n");
    nvlog_t ls, lc;
    ok("설정 · 계수 자리 열기", nvlog_mount(&ls, &OPS, PERSIST_SETTINGS_BASE) == NVLOG_OK &&
                                  nvlog_mount(&lc, &OPS, PERSIST_COUNTER_BASE) == NVLOG_OK);
    settings_t s;
    ok("설정 없음 → 기본값 (대역 7 = KR920, 영점 · 키 없음)",
       settings_load(&ls, &s) == NVLOG_E_EMPTY && s.band == 7 && !s.zero_set && !s.keys_set);
    counter_t c;
    ok("계수 없음 → 0", counter_load(&lc, &c) == NVLOG_E_EMPTY && c.total_nm3 == 0.0);
    ok("자리가 링(12 MB) 뒤", PERSIST_SETTINGS_BASE >= RING_DEFAULT_SECTORS * W25Q_SECTOR);

    printf("설정 저장 · 다시 켜서 읽기\n");
    settings_t w;
    settings_default(&w);
    w.zero_pa = -0.8125;
    w.vent_p_pa = 1962.0;
    w.zero_set = true;
    w.keys_set = true;
    for (int i = 0; i < 8; i++) {
        w.dev_eui[i] = (uint8_t)(0xA0 + i);
        w.app_eui[i] = (uint8_t)i;
    }
    for (int i = 0; i < 16; i++) {
        w.app_key[i] = (uint8_t)(0x11 * i);
    }
    w.batt_cal_mv = -23;
    w.sdp_off = true;
    ok("설정 저장", settings_save(&ls, &w) == NVLOG_OK);
    nvlog_t ls2;
    nvlog_mount(&ls2, &OPS, PERSIST_SETTINGS_BASE);
    settings_t r;
    ok("다시 켜서 읽은 값 = 저장한 값", settings_load(&ls2, &r) == NVLOG_OK && r.zero_pa == w.zero_pa &&
                                            r.vent_p_pa == w.vent_p_pa && r.band == 7 && r.zero_set && r.keys_set &&
                                            memcmp(r.dev_eui, w.dev_eui, 8) == 0 && memcmp(r.app_key, w.app_key, 16) == 0 &&
                                            r.batt_cal_mv == -23 && r.sdp_off && r.zero_set && r.keys_set);
    printf("옛 판(50 B, 배터리 보정 없음) 설정도 읽힘\n");
    {
        uint8_t old[50];
        uint8_t full[52];
        settings_pack(&w, full);
        memcpy(old, full, 50);
        nvlog_save(&ls2, old, 50);
        settings_t o;
        ok("보정값은 0 으로, 나머지는 그대로", settings_load(&ls2, &o) == NVLOG_OK && o.batt_cal_mv == 0 && o.sdp_off &&
                                                    o.zero_pa == w.zero_pa && o.keys_set);
        settings_save(&ls2, &w);
    }

    printf("계수 1분마다 저장 — 섹터 넘김 (섹터당 64칸)\n");
    for (uint32_t i = 0; i < 150; i++) {
        counter_t x = ctr(i);
        counter_save(&lc, &x);
    }
    nvlog_t lc2;
    nvlog_mount(&lc2, &OPS, PERSIST_COUNTER_BASE);
    const counter_t want = ctr(149);
    ok("150번 저장 뒤 다시 켜면 마지막(149번째) 값", counter_load(&lc2, &c) == NVLOG_OK && same_ctr(&c, &want));
    ok("순번 65535 다음은 0 으로 돎", ctr(6).report_seq == 0);
    ok("계수만 저장해도 설정은 그대로", settings_load(&ls2, &r) == NVLOG_OK && r.zero_pa == w.zero_pa);
    /* 1분마다 저장해도: 섹터 하나를 64분 · 64분 번갈아 지움 → 10만 번이면 */
    const double years = 64.0 * 2.0 * 100000.0 / (60.0 * 24.0 * 365.0);
    printf("       (1분마다 저장 때 섹터 수명 %.1f 년)\n", years);
    ok("1분마다 써도 섹터 수명 20년 넘음", years > 20.0);

    printf("쓰다가 전원 끊김\n");
    counter_t before;
    counter_load(&lc2, &before);
    C.cut_program_at = 20;
    counter_t x = ctr(500);
    counter_save(&lc2, &x);
    C.dead = false;
    nvlog_t lc3;
    nvlog_mount(&lc3, &OPS, PERSIST_COUNTER_BASE);
    ok("깨진 칸은 버리고 바로 앞 값", counter_load(&lc3, &c) == NVLOG_OK && same_ctr(&c, &before));
    x = ctr(501);
    ok("이어서 저장 · 읽기", counter_save(&lc3, &x) == NVLOG_OK && counter_load(&lc3, &c) == NVLOG_OK && same_ctr(&c, &x));

    printf("다른 섹터 지우다 전원 끊김\n");
    /* 지금 섹터를 끝까지 채운 뒤, 넘어가며 지우는 순간 끊기게 함 */
    while (lc3.cur_slot < lc3.slots) {
        x = ctr(600 + lc3.cur_slot);
        counter_save(&lc3, &x);
    }
    const counter_t last_full = x;
    const uint8_t full_sec = lc3.cur_sec;
    C.cut_erase_at = 1000;
    x = ctr(900);
    counter_save(&lc3, &x);
    C.dead = false;
    nvlog_t lc4;
    nvlog_mount(&lc4, &OPS, PERSIST_COUNTER_BASE);
    ok("앞 섹터의 마지막 값이 남아 있음", counter_load(&lc4, &c) == NVLOG_OK && same_ctr(&c, &last_full));
    ok("넘어갈 땐 꽉 찬 섹터가 아니라 다른 섹터를 지움", lc4.cur_sec == full_sec && lc4.cur_slot == lc4.slots);
    x = ctr(901);
    ok("다음 저장 때 그 섹터를 다시 지우고 씀", counter_save(&lc4, &x) == NVLOG_OK && counter_load(&lc4, &c) == NVLOG_OK &&
                                                     same_ctr(&c, &x) && lc4.cur_sec != full_sec);
    uint8_t raw[64];
    C.dead = false;
    w25q_read(&DEV, PERSIST_COUNTER_BASE + full_sec * W25Q_SECTOR + 63u * 64u, raw, sizeof(raw));
    ok("앞 섹터 마지막 칸은 지워지지 않고 남음", raw[0] == 'N' && raw[1] == 'V');
    nvlog_t lc5;
    nvlog_mount(&lc5, &OPS, PERSIST_COUNTER_BASE);
    ok("다시 켜도 그 값", counter_load(&lc5, &c) == NVLOG_OK && same_ctr(&c, &x));

    printf("써도 안 써지는 칸\n");
    C.stuck_program = 1;
    x = ctr(950);
    ok("한 칸 버리고 다음 칸에 씀", counter_save(&lc5, &x) == NVLOG_OK && counter_load(&lc5, &c) == NVLOG_OK && same_ctr(&c, &x));
    C.stuck_program = 2;
    ok("두 번 다 안 되면 확인 실패", counter_save(&lc5, &x) == NVLOG_E_VERIFY);
    C.stuck_program = 0;

    printf("바이트 모양 고정\n");
    uint8_t b[PERSIST_COUNTER_LEN];
    counter_t k = {0};
    k.total_nm3 = 1.0;
    k.report_seq = 0x1234;
    counter_pack(&k, b);
    ok("누적 1.0 = 00 00 00 00 00 00 F0 3F (IEEE 754, 작은 쪽 먼저)", b[6] == 0xF0 && b[7] == 0x3F && b[0] == 0);
    ok("순번 0x1234 → 20번째 바이트부터 34 12", b[20] == 0x34 && b[21] == 0x12);
    ok("너무 긴 내용은 거부", nvlog_save(&lc5, b, NVLOG_MAX_DATA + 1) == NVLOG_E_ARG);
    ok("섹터 경계 아닌 자리는 거부", nvlog_mount(&lc5, &OPS, PERSIST_COUNTER_BASE + 64) == NVLOG_E_ARG);

    test_edata();
    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
