#include <string.h>
#include "ds18b20.h"
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
    sdp810_init(&s->sdp);
}

void sensors_set_extra(sensors_t *s, const onewire_t *ow, uint32_t (*tick_ms)(void), uint32_t (*wall_ms)(void))
{
    s->ow = ow;
    s->tick_ms = tick_ms;
    s->wall_ms = wall_ms;
}

/* DS18B20: 변환이 끝났으면 읽고, 간격이 됐으면 새로 시작. 반환 = 지금 쓸 수 있는 값이 있나 */
static bool probe_step(sensors_t *s, sensors_result_t *r)
{
    r->e_probe = SENS_E_BUS;
    if (s->ow == NULL || s->wall_ms == NULL) {
        return false;
    }
    const uint32_t now = s->wall_ms();
    if (s->probe_pending && (uint32_t)(now - s->probe_start_ms) >= DS18B20_CONV_MS) {
        double t = 0.0;
        r->e_probe = ds18b20_read(s->ow, &t);
        s->probe_pending = false;
        if (r->e_probe == SENS_OK) {
            s->probe_t_c = t;
            s->probe_at_ms = now;
            s->probe_have = true;
        }
    }
    if (!s->probe_pending && (!s->probe_started_once || (uint32_t)(now - s->probe_start_ms) >= SENSORS_PROBE_PERIOD_MS)) {
        s->probe_started_once = true;
        s->probe_start_ms = now;
        s->probe_pending = (ds18b20_convert(s->ow) == SENS_OK);   /* 존재 펄스가 없으면 센서 없음 */
    }
    const bool fresh = s->probe_have && (uint32_t)(now - s->probe_at_ms) <= SENSORS_PROBE_MAX_AGE_MS;
    if (fresh && r->e_probe != SENS_OK) {
        r->e_probe = SENS_STALE;            /* 이번엔 안 읽었지만 최근 값이 있음 */
    }
    return fresh;
}

sens_err_t sensors_power_on(sensors_t *s)
{
    if (s->power != NULL) {
        s->power(true);
    }
    s->powered = true;
    s->bmp_retry_n = 0;
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

    /* SDP810 측정(45 ms)을 맨 먼저 시작해 두고 다른 센서를 읽는 동안 기다림 */
    const uint32_t sdp_t0 = s->tick_ms != NULL ? s->tick_ms() : 0u;
    r->e_sdp = sdp810_trigger(s->bus_pg, &s->sdp);

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

    if (!s->bmp_ready && s->powered && ++s->bmp_retry_n >= SENSORS_BMP_RETRY) {
        s->bmp_retry_n = 0;
        s->bmp_ready = bmp390_init(s->bus_pg, &s->bmp) == SENS_OK;   /* 켤 때 실패했으면 여기서 다시 */
        if (s->bmp_ready) {
            s->bmp_reinits++;
        }
    }
    bmp390_reading_t b = {0};
    r->e_baro = s->bmp_ready ? bmp390_measure(s->bus_pg, &s->bmp, &b) : SENS_E_BUS;
    if (r->e_baro == SENS_OK) {
        r->sample.patm_pa = b.p_pa;
        r->sample.t_gas_c = b.t_c;
        r->t_board_c = b.t_c;
        s->patm_fallback_pa = b.p_pa;
    } else {
        /* 기압은 천천히 변해서 마지막 값으로 대신해도 오차가 작음. 온도는 정압 센서 것으로 대신.
         * 한 번도 못 쟀으면 해수면 값이라 높은 곳에선 틀림 → patm_held 로 보고에 표시 */
        r->sample.patm_pa = s->patm_fallback_pa;
        r->sample.patm_held = true;
        r->sample.t_gas_c = (r->e_pg == SENS_OK) ? a.t_c : h.t_c;
    }

    /* SDP810: 시작하고 50 ms 를 채운 뒤 읽고 다시 재움 (측정 중엔 묻지 않음 — sdp810.h) */
    if (r->e_sdp == SENS_OK) {
        uint32_t wait = SDP810_MEAS_MS;
        if (s->tick_ms != NULL) {
            const uint32_t spent = s->tick_ms() - sdp_t0;
            wait = spent >= SDP810_MEAS_MS ? 0u : SDP810_MEAS_MS - spent;
        }
        if (wait > 0u) {
            s->bus_pg->delay_ms(s->bus_pg->ctx, wait);
        }
        sdp810_reading_t d = {0};
        r->e_sdp = sdp810_fetch(s->bus_pg, &s->sdp, &d);
        sdp810_sleep(s->bus_pg, &s->sdp);
        if (r->e_sdp == SENS_OK) {
            r->sample.dp2_ok = true;
            r->sample.dp2_pa = d.dp_pa;
            r->t_sdp_c = d.t_c;
        }
    }

    /* DS18B20: 있으면 가스 온도를 이 값으로 (프로브가 가스관에 닿음 — BMP390 은 함체 안 온도) */
    if (probe_step(s, r)) {
        r->sample.t_probe_ok = true;
        r->sample.t_probe_c = s->probe_t_c;
        r->sample.t_gas_c = s->probe_t_c;
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
