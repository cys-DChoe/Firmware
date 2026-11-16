/* 셀 전압 계산 맥 시험 */
#include <stdio.h>
#include "batt.h"

static int fails = 0, checks = 0;
static void ok(const char *what, int v)
{
    checks++;
    if (!v) fails++;
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

int main(void)
{
    printf("VDDA 구하기 (VREFINT)\n");
    /* VREFINT 1.212 V 를 3.3 V 에서 잰 공장값 ≈ 1504 */
    const uint16_t cal = 1504;
    ok("3.3 V 그대로면 3300 mV", batt_vdda_mv(1504, cal) == 3300u);
    ok("3V3 가 3.25 V 로 낮으면 원값 1527 → 3250 mV", batt_vdda_mv(1527, cal) == 3250u);
    ok("원값 0 → 0 (나눗셈 막음)", batt_vdda_mv(0, cal) == 0u);

    printf("셀 전압 (÷2 나눔)\n");
    ok("3.7 V 셀 · VDDA 3250 → 원값 2331 → 3700 mV", batt_cell_mv(2331, 3250, 0) == 3700u);
    printf("       (%u mV)\n", batt_cell_mv(2331, 3250, 0));
    ok("VDDA 를 3300 으로 잘못 가정했다면 3757 mV (+57) — VREFINT 로 이 오차를 없앰", batt_cell_mv(2331, 3300, 0) == 3757u);
    ok("1점 보정 −20 mV", batt_cell_mv(2331, 3250, -20) == 3680u);
    ok("한 눈금 ≈ 1.6 mV", batt_cell_mv(2332, 3250, 0) - batt_cell_mv(2331, 3250, 0) <= 2u);
    ok("원값 최대 → 6500 mV (VDDA × 2)", batt_cell_mv(4095, 3250, 0) == 6500u);
    ok("음수로 가면 0", batt_cell_mv(10, 3250, -500) == 0u);
    ok("문턱 3.5 V 근처: 원값 2205 → 3500 mV", batt_cell_mv(2205, 3250, 0) >= 3499u && batt_cell_mv(2205, 3250, 0) <= 3501u);

    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
