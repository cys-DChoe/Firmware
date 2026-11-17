#include <string.h>
#include "sensors.h"

static double clamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

void sensors_init(sensors_t *s, const i2c_bus_t *bus_dp, const i2c_bus_t *bus_pg, void (*power)(bool on))
{
    memset(s, 0, sizeof(*s));
    s->bus_dp = bus_dp;
    s->bus_pg = bus_pg;
    s->power = power;
    hsc_default_cfg(&s->hsc);
    abp2_default_cfg(&s->abp2);
    s->patm_fallback_pa = 101325.0;     /* 첫 측정 전 — 곧 실제 값으로 바뀜 */
}

sens_err_t sensors_power_on(sensors_t *s)
{
    if (s->power != NULL) {
        s->power(true);
    }
    s->powered = true;
    s->bus_pg->delay_ms(s->bus_pg->ctx, SENSORS_POWER_ON_MS);
    /* BMP390 은 전원이 꺼지면 설정이 지워지므로 켤 때마다 다시 설정 */
    const sens_err_t e = bmp390_init(s->bus_pg, &s->bmp);
    s->bmp_ready = (e == SENS_OK);
    return e;
}

void sensors_power_off(sensors_t *s)
{
    if (s->power != NULL) {
        s->power(false);
    }
    s->powered = false;
    s->bmp_ready = false;
}

void sensors_read(sensors_t *s, sensors_result_t *r)
{
    memset(r, 0, sizeof(*r));

    /* 정압 측정을 먼저 시작해 두고(5 ms) 그동안 차압을 읽음 */
    r->e_pg = abp2_start(s->bus_pg);

    hsc_reading_t h;
    r->e_dp = hsc_read(s->bus_dp, &s->hsc, &h);
    r->sample.dp_raw_pa = h.p_pa;
    r->t_hsc_c = h.t_c;

    abp2_reading_t a = {0};
    if (r->e_pg == SENS_OK) {
        s->bus_pg->delay_ms(s->bus_pg->ctx, ABP2_CONV_MS);
        r->e_pg = SENS_E_BUSY;
        for (int tries = 0; tries < 5 && r->e_pg == SENS_E_BUSY; tries++) {
            r->e_pg = abp2_fetch(s->bus_pg, &s->abp2, &a);
            if (r->e_pg == SENS_E_BUSY) {
                s->bus_pg->delay_ms(s->bus_pg->ctx, 1);
            }
        }
    }
    r->sample.pg_pa = a.p_pa;
    r->t_abp2_c = a.t_c;

    bmp390_reading_t b = {0};
    r->e_baro = s->bmp_ready ? bmp390_measure(s->bus_pg, &s->bmp, &b) : SENS_E_BUS;
    if (r->e_baro == SENS_OK) {
        r->sample.patm_pa = b.p_pa;
        r->sample.t_gas_c = b.t_c;
        s->patm_fallback_pa = b.p_pa;
    } else {
        /* 기압은 천천히 변해서 마지막 값으로 대신해도 오차가 작음. 온도는 정압 센서 것으로 대신. */
        r->sample.patm_pa = s->patm_fallback_pa;
        r->sample.t_gas_c = (r->e_pg == SENS_OK) ? a.t_c : h.t_c;
    }

    /* 센서 범위(출력 10–90 %)를 벗어난 값도 버리지 않고 끝값으로 잘라 씀:
     *   정압이 0 근처(빈 소화조 · 책상 시험)면 ABP2 가 10 % 아래로 살짝 내려가는데, 버리면 그 초가 통째로 빔.
     *   차압이 만점을 넘으면(큰 불꽃) 버리면 사용량이 0 으로 빠짐 → 만점으로 세고, 계산 모듈이 '범위 밖' 비트를 켬. */
    if (r->e_dp == SENS_E_RANGE) {
        r->sample.dp_raw_pa = clamp(h.p_pa, s->hsc.p_min_pa, s->hsc.p_max_pa);
        r->clipped = true;
    }
    if (r->e_pg == SENS_E_RANGE) {
        r->sample.pg_pa = clamp(a.p_pa, s->abp2.p_min_pa, s->abp2.p_max_pa);
        r->clipped = true;
    }
    /* 차압 · 정압이 정상(또는 이전 값 · 끝값)일 때 사용량 계산에 넣음. 센서 오류 · 응답 없음만 버림 */
    r->usable = (r->e_dp == SENS_OK || r->e_dp == SENS_STALE || r->e_dp == SENS_E_RANGE) &&
                (r->e_pg == SENS_OK || r->e_pg == SENS_E_RANGE);
}
