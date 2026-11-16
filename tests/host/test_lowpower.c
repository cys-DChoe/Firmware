/* 잠 모드 계산 맥 시험 — 깨우기 타이머 값 · LPUART 속도 값 · 달력 ↔ Unix 초 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lowpower.h"

static int fails = 0, checks = 0;

static void ok(const char *what, bool v)
{
    checks++;
    if (!v) {
        fails++;
    }
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

int main(void)
{
    printf("깨는 주기 (설계서 결정 ①)\n");
    ok("흐름 있으면 1 s", lp_tick_ms(true) == 1000u);
    ok("평소 10 s", lp_tick_ms(false) == 10000u);

    printf("RTC 깨우기 타이머 (LSE ÷ 16 = 2048 Hz)\n");
    uint32_t r = 0;
    ok("1 s → 2047", lp_wut_reload(1000, &r) && r == 2047u);
    ok("10 s → 20479", lp_wut_reload(10000, &r) && r == 20479u);
    ok("10 s 실제 = 10.000000 s", lp_wut_actual_us(20479u) == 10000000u);
    ok("32 s → 65535 (최대)", lp_wut_reload(32000, &r) && r == 65535u);
    ok("33 s 는 안 됨", !lp_wut_reload(33000, &r));
    ok("0 ms 는 안 됨", !lp_wut_reload(0, &r));
    ok("1 ms → 1틱(0.49 ms)으로 반올림", lp_wut_reload(1, &r) && r == 1u);

    printf("LoRa UART 속도 값 (LPUART BRR = 256 × 커널 / 속도)\n");
    uint32_t brr = 0;
    int32_t ppm = 0;
    ok("LSE 32768 Hz · 9600 → BRR 874, 오차 300 ppm 안", lp_lpuart_brr(32768u, 9600u, &brr, &ppm) && brr == 874u &&
                                                               labs((long)ppm) < 300);
    printf("       (실제 오차 %d ppm)\n", ppm);
    ok("LSE · 115200 은 안 됨 (커널 < 3 × 속도)", !lp_lpuart_brr(32768u, 115200u, &brr, &ppm));
    ok("LSE · 19200 도 안 됨 (BRR 437 < 0x300)", !lp_lpuart_brr(32768u, 19200u, &brr, &ppm));
    ok("PCLK3 32 MHz · 115200 → BRR 71111", lp_lpuart_brr(32000000u, 115200u, &brr, &ppm) && brr == 71111u);
    ok("PCLK3 32 MHz · 9600 → BRR 853333", lp_lpuart_brr(32000000u, 9600u, &brr, &ppm) && brr == 853333u);
    ok("PCLK3 32 MHz · 300 은 안 됨 (커널 > 4096 × 속도)", !lp_lpuart_brr(32000000u, 300u, &brr, &ppm));

    printf("달력 ↔ Unix 초\n");
    lp_civil_t c = {2026, 11, 16, 9, 0, 0};
    ok("2026-11-16 09:00:00 UTC = 1794819600", lp_unix_from_civil(&c) == 1794819600u);
    lp_civil_t e = {2000, 1, 1, 0, 0, 0};
    ok("2000-01-01 = 946684800", lp_unix_from_civil(&e) == 946684800u);
    lp_civil_t l = {2028, 2, 29, 23, 59, 59};
    lp_civil_t back;
    lp_civil_from_unix(lp_unix_from_civil(&l), &back);
    ok("윤일 2028-02-29 23:59:59 왕복", back.year == 2028 && back.month == 2 && back.day == 29 && back.hour == 23 &&
                                            back.min == 59 && back.sec == 59);
    /* 맥 표준 라이브러리(gmtime)와 2000–2099 사이 1만 개 대조 */
    int mism = 0;
    srand(7);
    for (int i = 0; i < 10000; i++) {
        const uint32_t t = 946684800u + (uint32_t)(((uint64_t)rand() * 3155760000ull) / RAND_MAX);
        lp_civil_t x;
        lp_civil_from_unix(t, &x);
        const time_t tt = (time_t)t;
        struct tm g;
        gmtime_r(&tt, &g);
        if (x.year != g.tm_year + 1900 || x.month != g.tm_mon + 1 || x.day != g.tm_mday || x.hour != g.tm_hour ||
            x.min != g.tm_min || x.sec != g.tm_sec || lp_unix_from_civil(&x) != t) {
            mism++;
        }
    }
    ok("gmtime 과 1만 개 대조 · 왕복 모두 같음", mism == 0);

    printf("1분 경계\n");
    ok("같은 분이면 아님", !lp_minute_due(1794819600u / 60u, 1794819659u));
    ok("다음 분이면 보고", lp_minute_due(1794819600u / 60u, 1794819660u));

    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
