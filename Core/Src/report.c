/* 1분 보고 묶기 — 형식은 report.h 표 */
#include <math.h>
#include "report.h"

/* 눈금 (report.h 표와 같아야 함) */
#define K_TOTAL     10000.0     /* 0.0001 Nm³ */
#define K_FLOW      1000.0      /* 1 L/h = 0.001 Nm³/h */
#define K_DP        20.0        /* 0.05 Pa */
#define K_T         10.0        /* 0.1 °C */
#define PATM_OFS    50000.0
#define BATT_OFS    2500u
#define BATT_STEP   10u

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* 반올림해서 [lo, hi] 안으로. 잘렸으면 *clip = true. NaN 은 0 으로 두고 잘림 표시. */
static double fit(double x, double lo, double hi, bool *clip)
{
    if (isnan(x)) {
        *clip = true;
        return 0.0;
    }
    double r = round(x);
    if (r < lo) { *clip = true; return lo; }
    if (r > hi) { *clip = true; return hi; }
    return r;
}

size_t report_encode(const report_t *r, uint8_t out[REPORT_LEN])
{
    bool clip = false;
    const uint32_t total = (uint32_t)fit(r->total_nm3 * K_TOTAL, 0.0, 4294967295.0, &clip);
    const uint16_t flow = (uint16_t)fit(r->flow_nm3h * K_FLOW, 0.0, 65535.0, &clip);
    const int16_t pg = (int16_t)fit(r->pg_pa, -32768.0, 32767.0, &clip);
    const int16_t dp = (int16_t)fit(r->dp_pa * K_DP, -32768.0, 32767.0, &clip);
    const int16_t t = (int16_t)fit(r->t_c * K_T, -32768.0, 32767.0, &clip);
    const uint16_t patm = (uint16_t)fit(r->patm_pa - PATM_OFS, 0.0, 65535.0, &clip);
    double b = ((double)r->batt_mv - BATT_OFS) / BATT_STEP;
    const uint8_t batt = (uint8_t)fit(b, 0.0, 255.0, &clip);

    uint16_t flags = r->flags;
    if (clip) {
        flags |= REPORT_F_CLIPPED;
    }

    out[0] = REPORT_VERSION;
    put16(&out[1], r->seq);
    put32(&out[3], r->unix_s);
    put32(&out[7], total);
    put16(&out[11], flow);
    put16(&out[13], (uint16_t)pg);
    put16(&out[15], (uint16_t)dp);
    put16(&out[17], (uint16_t)t);
    put16(&out[19], patm);
    out[21] = batt;
    put16(&out[22], flags);
    return REPORT_LEN;
}

bool report_decode(const uint8_t *in, size_t len, report_t *r)
{
    if (len != REPORT_LEN || in[0] != REPORT_VERSION) {
        return false;
    }
    r->seq = get16(&in[1]);
    r->unix_s = get32(&in[3]);
    r->total_nm3 = get32(&in[7]) / K_TOTAL;
    r->flow_nm3h = get16(&in[11]) / K_FLOW;
    r->pg_pa = (int16_t)get16(&in[13]);
    r->dp_pa = (int16_t)get16(&in[15]) / K_DP;
    r->t_c = (int16_t)get16(&in[17]) / K_T;
    r->patm_pa = get16(&in[19]) + PATM_OFS;
    r->batt_mv = (uint16_t)(in[21] * BATT_STEP + BATT_OFS);
    r->flags = get16(&in[22]);
    return true;
}

/* 펌웨어 주의 7절: 3.5 V 경고 · 3.35 V 품질 B, 경계에서 0.1 V 폭(±50 mV)으로 오르내림 */
static bool hysteresis(uint16_t mv, uint16_t limit_mv, bool was_low)
{
    return was_low ? (mv < limit_mv + 50u) : (mv < limit_mv - 50u);
}

uint16_t report_battery_flags(uint16_t batt_mv, uint16_t prev_flags)
{
    uint16_t f = 0;
    if (hysteresis(batt_mv, 3500u, (prev_flags & REPORT_F_BATT_WARN) != 0)) {
        f |= REPORT_F_BATT_WARN;
    }
    if (hysteresis(batt_mv, 3350u, (prev_flags & REPORT_F_BATT_QUALITY) != 0)) {
        f |= REPORT_F_BATT_QUALITY;
    }
    return f;
}
