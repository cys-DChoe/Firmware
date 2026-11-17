/* LTE 켜기 · 망 확인 · 끄기 맥 시험 (가짜 EG915U) */
#include <stdio.h>
#include <string.h>
#include "fake_lte.h"
#include "lte.h"

static int fails = 0, checks = 0;
static void ok(const char *what, bool v)
{
    checks++;
    if (!v) {
        fails++;
    }
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

static uint32_t now;
static fake_lte_t F;
static lte_t L;

static void fresh(void)
{
    now = 1000;
    fake_lte_new(&F, &now);
    lte_io_t io;
    fake_lte_io(&F, &io);
    lte_init(&L, &io);
}

/* 1 s 마다 부름. 끝날 때까지 (최대 limit 초) */
static uint32_t run(uint32_t limit)
{
    const uint32_t t0 = now;
    while (lte_step(&L, now) && now - t0 < limit) {
        now++;
    }
    return now - t0;
}

int main(void)
{
    printf("정상: 켜고 → 유심 → 망 등록 → 신호 · 통신사 → AT+QPOWD 로 끔\n");
    fresh();
    ok("시작", lte_start(&L, now));
    ok("하는 중엔 다시 시작 안 됨", !lte_start(&L, now));
    ok("PWRKEY 누름", F.key_down);
    run(600);
    ok("결과: 망 등록됨 · 집 망(1)", L.done && L.res.code == LTE_R_OK && L.res.reg == 1u);
    ok("신호 18 (−77 dBm) · 통신사 이름", L.res.csq == 18u && strcmp(L.res.oper, "SKTelecom") == 0);
    ok("AT+QPOWD 로 꺼짐 · STATUS 낮음 · 1.8 V 핀 꺼둠 · PWRKEY 놓음", L.res.off == LTE_OFF_QPOWD && !F.on && !F.io &&
                                                                         !F.key_down);
    ok("켜서 끌 때까지 1분 안 (등록 20 s 가정)", L.res.secs > 20u && L.res.secs < 60u);
    printf("       (걸린 시간 %u s)\n", (unsigned)L.res.secs);
    uint8_t b[7];
    lte_result_encode(&L.res, b);
    ok("LoRa 로 올릴 7 바이트: 01 00 01 01 12 00 secs", b[0] == 1 && b[1] == 0 && b[2] == 1 && b[3] == 1 && b[4] == 18 &&
                                                       ((b[5] << 8) | b[6]) == L.res.secs);

    printf("로밍(5) 도 등록으로 봄\n");
    fresh();
    F.reg_stat = 5;
    lte_start(&L, now);
    run(600);
    ok("로밍 → 등록됨", L.res.code == LTE_R_OK && L.res.reg == 5u);

    printf("유심 없음\n");
    fresh();
    F.sim = false;
    lte_start(&L, now);
    run(600);
    ok("유심 없음 → AT 로 끔", L.res.code == LTE_R_NO_SIM && L.res.off == LTE_OFF_QPOWD && !F.on);

    printf("망 등록이 안 됨 (3분 기다림)\n");
    fresh();
    F.reg_after = 100000;
    lte_start(&L, now);
    const uint32_t took = run(1000);
    ok("등록 안 됨 → 신호는 읽고 끔", L.res.code == LTE_R_NO_NET && L.res.reg == 2u && L.res.csq == 18u && !F.on);
    ok("약 3분 기다리고 그만둠", took >= LTE_REG_S && took < LTE_REG_S + 60u);

    printf("모듈이 안 켜짐 (전원 없음)\n");
    fresh();
    F.dead = true;
    lte_start(&L, now);
    run(600);
    ok("안 켜짐 → 끌 것도 없음", L.res.code == LTE_R_NO_POWER && L.res.off == LTE_OFF_NONE && !F.key_down);

    printf("1.8 V(VDDIO2)가 안 올라옴\n");
    fresh();
    F.no_vddio2 = true;
    lte_start(&L, now);
    run(600);
    ok("UART 를 못 켬 → PWRKEY 로 끔", L.res.code == LTE_R_NO_IO && L.res.off == LTE_OFF_KEY && !F.on);

    printf("AT+QPOWD 를 안 들음 → PWRKEY 로 끔\n");
    fresh();
    F.qpowd_ignored = true;
    lte_start(&L, now);
    run(600);
    ok("PWRKEY 로 꺼짐", L.res.code == LTE_R_OK && L.res.off == LTE_OFF_KEY && !F.on && F.presses == 2);

    printf("AT · PWRKEY 둘 다 안 들음 → 리셋 뒤 PWRKEY\n");
    fresh();
    F.qpowd_ignored = true;
    F.key_ignored_until_reset = true;
    lte_start(&L, now);
    run(900);
    ok("리셋 한 번 뒤 꺼짐", L.res.off == LTE_OFF_RESET && !F.on && F.resets == 1 && !F.rst_down);

    printf("아무것도 안 들음 → 못 끔으로 남김\n");
    fresh();
    F.qpowd_ignored = true;
    F.key_off_ignored = true;
    lte_start(&L, now);
    run(900);
    ok("못 끔 표시 · 일은 끝냄", !lte_busy(&L) && L.res.off == LTE_OFF_FAILED && F.on && F.resets == 1);

    printf("중간에 멈추라고 함 (셀 낮음)\n");
    fresh();
    F.reg_after = 100000;
    lte_start(&L, now);
    run(15);
    lte_stop(&L);
    const uint32_t t_stop = run(600);
    ok("멈춤 → 바로 AT 로 끔", L.res.code == LTE_R_ABORTED && L.res.off == LTE_OFF_QPOWD && !F.on && t_stop < 15u);
    fresh();
    lte_start(&L, now);
    lte_stop(&L);                                   /* 켜지는 중에 멈춤 */
    run(600);
    ok("켜지는 중에 멈춰도 결국 꺼짐", L.res.code == LTE_R_ABORTED && !F.on && !lte_busy(&L));

    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
