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
    ok("설정 저장", settings_save(&ls, &w) == NVLOG_OK);
    nvlog_t ls2;
    nvlog_mount(&ls2, &OPS, PERSIST_SETTINGS_BASE);
    settings_t r;
    ok("다시 켜서 읽은 값 = 저장한 값", settings_load(&ls2, &r) == NVLOG_OK && r.zero_pa == w.zero_pa &&
                                            r.vent_p_pa == w.vent_p_pa && r.band == 7 && r.zero_set && r.keys_set &&
                                            memcmp(r.dev_eui, w.dev_eui, 8) == 0 && memcmp(r.app_key, w.app_key, 16) == 0);

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

    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
