/* 1분 보고 묶기 맥 시험 — 묶고 풀어서 같은지, 자르기, 배터리 비트
 * 시험 벡터를 파일로 써서 파이썬 풀기(tools/report_decode.py)와도 대조함.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "report.h"

static int fails = 0, checks = 0;

static void ok(const char *what, bool v)
{
    checks++;
    if (!v) {
        fails++;
    }
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

static bool close(double a, double b, double tol) { return fabs(a - b) <= tol + 1e-12; }

static void write_vector(FILE *f, const report_t *r)
{
    uint8_t b[REPORT_LEN];
    report_encode(r, b);
    for (unsigned i = 0; i < REPORT_LEN; i++) {
        fprintf(f, "%02x", b[i]);
    }
    report_t d;
    report_decode(b, REPORT_LEN, &d);   /* 잘림 비트가 붙었을 수 있으니 실제 flags 는 풀어 낸 값 */
    fprintf(f, "|%u|%u|%.6f|%.6f|%.3f|%.4f|%.3f|%.3f|%u|%u\n", r->seq, r->unix_s, r->total_nm3, r->flow_nm3h,
            r->pg_pa, r->dp_pa, r->t_c, r->patm_pa, r->batt_mv, d.flags);
}

int main(int argc, char **argv)
{
    printf("크기\n");
    ok("24바이트 (LoRa 한도 32 안)", REPORT_LEN == 24 && REPORT_LEN <= 32);

    printf("평소 값: 묶고 풀면 눈금 안에서 같음\n");
    report_t r = {
        .seq = 258, .unix_s = 1794816000u, .total_nm3 = 123.4567, .flow_nm3h = 0.372,
        .pg_pa = 300.0, .dp_pa = 32.4, .t_c = 22.0, .patm_pa = 82501.3, .batt_mv = 3912,
        .flags = REPORT_F_FLOWING,
    };
    uint8_t b[REPORT_LEN];
    ok("길이 24 를 돌려줌", report_encode(&r, b) == REPORT_LEN);
    ok("첫 바이트 = 형식 버전 1", b[0] == 1);
    ok("순번 258 = 01 02 (큰 쪽 먼저)", b[1] == 0x01 && b[2] == 0x02);
    report_t d;
    ok("풀기 성공", report_decode(b, REPORT_LEN, &d));
    ok("순번 · 시각 그대로", d.seq == 258 && d.unix_s == 1794816000u);
    ok("누적 0.0001 Nm³ 눈금", close(d.total_nm3, 123.4567, 0.00005));
    ok("유량 1 L/h 눈금", close(d.flow_nm3h, 0.372, 0.0005));
    ok("정압 1 Pa", close(d.pg_pa, 300.0, 0.5));
    ok("차압 0.05 Pa", close(d.dp_pa, 32.4, 0.025));
    ok("온도 0.1 °C", close(d.t_c, 22.0, 0.05));
    ok("대기압 1 Pa", close(d.patm_pa, 82501.3, 0.5));
    ok("배터리 10 mV", d.batt_mv == 3910 || d.batt_mv == 3920);
    ok("상태 비트 그대로 (잘림 없음)", d.flags == REPORT_F_FLOWING);

    printf("누설 크기의 작은 차압도 남음\n");
    r.dp_pa = 0.4;
    report_encode(&r, b);
    report_decode(b, REPORT_LEN, &d);
    ok("0.4 Pa → 0.40 Pa", close(d.dp_pa, 0.4, 1e-9));

    printf("음수 값\n");
    r.pg_pa = -50.0;
    r.dp_pa = -1.35;
    r.t_c = -5.3;
    report_encode(&r, b);
    report_decode(b, REPORT_LEN, &d);
    ok("정압 −50 Pa", close(d.pg_pa, -50.0, 0.5));
    ok("차압 −1.35 Pa", close(d.dp_pa, -1.35, 0.025));
    ok("온도 −5.3 °C", close(d.t_c, -5.3, 0.05));

    printf("범위를 넘으면 끝값으로 자르고 CLIPPED 표시\n");
    report_t big = r;
    big.dp_pa = 2000.0;             /* HSC 만점 1,000 Pa 를 크게 넘는 이상값 */
    report_encode(&big, b);
    report_decode(b, REPORT_LEN, &d);
    ok("차압 1,638.35 Pa 에서 잘림", close(d.dp_pa, 1638.35, 1e-9));
    ok("CLIPPED 켜짐", (d.flags & REPORT_F_CLIPPED) != 0);
    big = r;
    big.patm_pa = NAN;              /* 기압 센서 실패 */
    report_encode(&big, b);
    report_decode(b, REPORT_LEN, &d);
    ok("NaN 은 잘림 표시", (d.flags & REPORT_F_CLIPPED) != 0);
    big = r;
    big.batt_mv = 2000;
    report_encode(&big, b);
    report_decode(b, REPORT_LEN, &d);
    ok("배터리 2.0 V 는 2.50 V 로 잘림", d.batt_mv == 2500 && (d.flags & REPORT_F_CLIPPED));

    printf("잘못된 입력은 거부\n");
    ok("길이 23 거부", !report_decode(b, 23, &d));
    uint8_t v2[REPORT_LEN];
    memcpy(v2, b, REPORT_LEN);
    v2[0] = 2;
    ok("모르는 버전 거부", !report_decode(v2, REPORT_LEN, &d));

    printf("배터리 비트 (3.5 V 경고 · 3.35 V 품질, 오르내림 폭 ±50 mV)\n");
    uint16_t f = 0;
    f = report_battery_flags(3460, f);
    ok("3.46 V: 아직 경고 아님 (내려갈 땐 3.45 V 부터)", f == 0);
    f = report_battery_flags(3440, f);
    ok("3.44 V: 경고", f == REPORT_F_BATT_WARN);
    f = report_battery_flags(3540, f);
    ok("3.54 V: 경고 유지 (다시 풀리려면 3.55 V)", f == REPORT_F_BATT_WARN);
    f = report_battery_flags(3560, f);
    ok("3.56 V: 경고 풀림", f == 0);
    f = report_battery_flags(3290, f);
    ok("3.29 V: 경고 + 품질", f == (REPORT_F_BATT_WARN | REPORT_F_BATT_QUALITY));

    /* 파이썬 대조용 벡터 */
    if (argc > 1) {
        FILE *vf = fopen(argv[1], "w");
        if (vf != NULL) {
            report_t a = {1, 1794816000u, 0.0, 0.0, 0.0, 0.0, 0.0, 101325.0, 4200, 0};
            write_vector(vf, &a);
            report_t b2 = {258, 1794816060u, 123.4567, 0.372, 300.0, 32.4, 22.0, 82501.3, 3912,
                           REPORT_F_FLOWING | REPORT_F_LEAK_B};
            write_vector(vf, &b2);
            report_t c = {65535, 4102444800u, 429496.0, 2.5, -120.0, -0.35, -12.4, 50000.0, 3350,
                          REPORT_F_VENTING | REPORT_F_BATT_WARN | REPORT_F_BATT_QUALITY | REPORT_F_RESENT};
            write_vector(vf, &c);
            report_t e = {7, 1794816120u, 0.5100, 0.0544, 1960.0, 999.95, 35.5, 115535.0, 5050,
                          REPORT_F_DEPLETED | REPORT_F_OUT_OF_RANGE | REPORT_F_ZERO_UNSET |
                          REPORT_F_TIME_UNSYNCED | REPORT_F_INCOMPLETE};
            write_vector(vf, &e);
            fclose(vf);
        }
    }

    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
