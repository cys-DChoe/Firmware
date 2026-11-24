/* 계량기 계산 모듈 — 설명은 meter.h, 식의 근거는 계산식 문서 절 번호로 적음 */
#include <math.h>
#include <string.h>
#include "meter.h"

/* ---------- 순수 식 ---------- */

/* 2절: ρ = P_abs × M ÷ (R × T) */
double meter_gas_density(double p_abs_pa, double t_k, double molar_kg_mol)
{
    return p_abs_pa * molar_kg_mol / (METER_R_J_MOL_K * t_k);
}

/* 2절: Q = C × Aₜ × √(2ΔP/ρ) ÷ √(1 − β⁴)  [m³/s]. 음수 차압은 흐름 0 으로 봄. */
double meter_flow_m3s(double dp_pa, double rho, double c, double throat_d_m, double inlet_d_m)
{
    if (dp_pa <= 0.0 || rho <= 0.0) {
        return 0.0;
    }
    const double area = M_PI * throat_d_m * throat_d_m / 4.0;
    const double beta = throat_d_m / inlet_d_m;
    const double b4 = beta * beta * beta * beta;
    return c * area * sqrt(2.0 * dp_pa / rho) / sqrt(1.0 - b4);
}

double meter_gas_viscosity(const meter_cfg_t *c, double t_k)
{
    const double s = c->visc_sutherland_k;
    return c->visc_20c_pa_s * pow(t_k / 293.15, 1.5) * (293.15 + s) / (t_k + s);
}

/* C(Re) — Re 는 고정 C(discharge_c, 0.97)로 낸 유량 Q₀ 로 잡음: 시험에서 C 를 그렇게 맞췄으므로 되풀이 계산이 필요 없음 */
double meter_discharge_c(const meter_cfg_t *c, double dp_pa, double rho, double t_k, double *re_out)
{
    if (re_out != NULL) {
        *re_out = 0.0;
    }
    if (c->c_re_a <= 0.0) {
        return c->discharge_c;
    }
    const double q0 = meter_flow_m3s(dp_pa, rho, c->discharge_c, c->throat_d_m, c->inlet_d_m);
    const double mu = meter_gas_viscosity(c, t_k);
    double re = (q0 > 0.0 && mu > 0.0) ? 4.0 * rho * q0 / (M_PI * c->throat_d_m * mu) : 0.0;
    if (re < c->c_re_min) {
        re = c->c_re_min;                   /* 시험 범위 아래는 끝 값 (흐름 0 이어도 같은 C) */
    }
    if (re_out != NULL) {
        *re_out = re;
    }
    const double cd = re > 0.0 ? c->c_re_a + c->c_re_b / re : c->discharge_c;
    /* 벤치 venturi.py 처럼 0.5–1.05 밖이면 식을 믿지 않고 고정 C (계수를 잘못 넣었을 때 · 검수 9번) */
    return (cd < METER_C_MIN || cd > METER_C_MAX) ? c->discharge_c : cd;
}

/* 3절: Qₙ = Q × (P_abs ÷ 101,325) × (273.15 ÷ T) — 그 곱하는 몫 */
double meter_normal_factor(double p_abs_pa, double t_k)
{
    return (p_abs_pa / METER_P_STD_PA) * (METER_T_STD_K / t_k);
}

/* 계산식 문서 계산기 1 과 같은 고도 → 대기압 식 (표준 대기) */
double meter_patm_from_altitude(double alt_m)
{
    return METER_P_STD_PA * pow(1.0 - 2.25577e-5 * alt_m, 5.25588);
}

static double scratch[METER_EVT_BINS > METER_IDLE_SLOTS ? METER_EVT_BINS : METER_IDLE_SLOTS];

double meter_median(const double *v, uint16_t n)
{
    if (n == 0) {
        return NAN;
    }
    memcpy(scratch, v, (size_t)n * sizeof(double));
    for (uint16_t i = 1; i < n; i++) {            /* 삽입 정렬 — n 이 작아서 충분 */
        double x = scratch[i];
        int j = (int)i - 1;
        while (j >= 0 && scratch[j] > x) {
            scratch[j + 1] = scratch[j];
            j--;
        }
        scratch[j + 1] = x;
    }
    return (n & 1u) ? scratch[n / 2] : 0.5 * (scratch[n / 2 - 1] + scratch[n / 2]);
}

/* ---------- 설정 ---------- */

void meter_default_cfg(meter_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    /* 벤투리 — 벤치 실물(11/19 확인: 입구 16 mm · 목 4 mm, β 0.25) + 11/20 가스계량기 비교 시험 HSC 식
     * (bench/server/venturi_cal.json "hsc": inv, a 0.97739 · b −158.91 · re_min 1725, 시험 Re 약 1,700–5,900, 6점 ±0.3 %) */
    c->throat_d_m = 4.0e-3;
    c->inlet_d_m = 16.0e-3;
    c->discharge_c = 0.97;          /* Q₀ (Re 잡기) 용 */
    c->discharge_c_low = 0.97;      /* c_re 를 끄면 쓰는 저유량 C */
    c->c_re_a = 0.97739;
    c->c_re_b = -158.91;
    c->c_re_min = 1725.0;
    /* 바이오가스(CH₄ 60 % · CO₂ 40 %) 점성: 문헌값 CH₄ 1.10e-5 · CO₂ 1.47e-5 Pa·s(20 °C)를 Wilke 식으로 섞음 → 1.318e-5,
     * 0–50 °C 에 맞춘 S 220 K (오차 0.003 % 미만). 현장 가스 조성이 다르면 바꿈. 공기(벤치)는 1.8133e-5 · 110.4 */
    c->visc_20c_pa_s = 1.318e-5;
    c->visc_sutherland_k = 220.0;
    c->gas_molar_kg_mol = 0.6 * 0.01604 + 0.4 * 0.04401;   /* 0.02723 */

    c->event_start_m3h = 0.05;
    c->cutoff_m3h = 0.05;
    c->event_start_hold_s = 30.0;
    c->event_end_m3h = 0.03;
    c->event_end_hold_s = 60.0;

    c->depl_p_ratio = 0.3;
    c->depl_flow_ratio = 0.5;
    c->depl_hold_s = 60.0;
    c->idle_window_min = 120;

    c->vent_p_pa = 98.1 * 20.0;     /* 물트랩 20 cm 가정 — 설치 때 입력 */
    c->vent_delta_min_pa = 50.0;
    c->vent_delta_frac = 0.05;
    c->vent_flat_pa = 5.0;
    c->vent_flat_min = 10;

    c->leak_window_min = 30;
    c->leak_windows = 4;
    c->leak_dp_min_pa = 0.3;        /* 임시 — 센서 잡음을 벤치에서 재고 정함 */

    c->dp_full_scale_pa = 1000.0;   /* HSC ±10 mbar */
    c->pg_full_scale_pa = 6000.0;   /* ABP2DRNT006KG: 0–6 kPa 게이지 (스펙 시트대로 확정 11/17, 다른 품번은 설정 명령으로) */

    c->zero_track_tau_s = 600.0;    /* 10분 — 10 s 간격 한 번 읽기 흔들림(0.24 Pa)이 영점에 약 0.02 Pa 로 남음 */
    c->zero_track_band_pa = 1.0;    /* 벤치 HSC_TRACK_BAND_PA */
    c->zero_track_diff_pa = 5.0;    /* 벤치와 같음 — 더 다르면 한쪽만 압력을 받는 중 */
    c->zero_track_max_pa = 1.0;     /* 버튼 영점 ±1 Pa 안에서만 (검수 7번) */
}

void meter_init(meter_t *m, const meter_cfg_t *cfg)
{
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    m->lw_valid = true;
    if (m->cfg.idle_window_min > METER_IDLE_SLOTS) {
        m->cfg.idle_window_min = METER_IDLE_SLOTS;
    }
    if (m->cfg.vent_flat_min >= METER_FLAT_SLOTS) {
        m->cfg.vent_flat_min = METER_FLAT_SLOTS - 1;
    }
}

void meter_set_zero(meter_t *m, double zero_pa)
{
    m->zero_pa = zero_pa;
    m->zero_base_pa = zero_pa;      /* 따라가기는 여기서 ±zero_track_max_pa 안 */
}

bool meter_is_flowing(const meter_t *m)
{
    return m->evt_active || m->above_s > 0.0 || m->last_flow_m3h > m->cfg.event_start_m3h;
}

/* ---------- 사용 이벤트 (4절) ---------- */

static void evt_bins_clear(meter_t *m)
{
    m->evt_nbins = 0;
    m->evt_bin_head = 0;
    m->bin_acc_flow = 0.0;
    m->bin_acc_s = 0.0;
}

static void evt_bins_add(meter_t *m, double flow_m3h, double dt)
{
    m->bin_acc_flow += flow_m3h * dt;
    m->bin_acc_s += dt;
    if (m->bin_acc_s >= METER_EVT_BIN_S) {
        m->evt_bins[m->evt_bin_head] = m->bin_acc_flow / m->bin_acc_s;
        m->evt_bin_head = (uint16_t)((m->evt_bin_head + 1u) % METER_EVT_BINS);
        if (m->evt_nbins < METER_EVT_BINS) {
            m->evt_nbins++;
        }
        m->bin_acc_flow = 0.0;
        m->bin_acc_s = 0.0;
    }
}

static void evt_step(meter_t *m, double q_h, double dv, double dvn, double dt)
{
    const meter_cfg_t *c = &m->cfg;
    if (!m->evt_active) {
        if (q_h > c->event_start_m3h) {
            /* 문턱을 넘은 순간부터 쌓아 두고, 30 s 이어지면 그 순간을 시작으로 확정 */
            m->above_s += dt;
            m->cand_m3 += dv;
            m->cand_nm3 += dvn;
            m->cand_s += dt;
            evt_bins_add(m, q_h, dt);
            if (m->above_s >= c->event_start_hold_s) {
                m->evt_active = true;
                m->evt_m3 = m->cand_m3;
                m->evt_nm3 = m->cand_nm3;
                m->evt_s = m->cand_s;
                m->below_s = 0.0;
                m->tail_m3 = m->tail_nm3 = m->tail_s = 0.0;
            }
        } else {
            m->above_s = 0.0;
            m->cand_m3 = m->cand_nm3 = m->cand_s = 0.0;
            evt_bins_clear(m);
        }
        return;
    }

    m->evt_m3 += dv;
    m->evt_nm3 += dvn;
    m->evt_s += dt;
    evt_bins_add(m, q_h, dt);
    if (q_h < c->event_end_m3h) {
        /* 밑돈 순간을 끝으로 봄 — 그 뒤 몫은 꼬리로 빼 둠 */
        m->below_s += dt;
        m->tail_m3 += dv;
        m->tail_nm3 += dvn;
        m->tail_s += dt;
        if (m->below_s >= c->event_end_hold_s) {
            m->events.count++;
            m->events.last_volume_m3 = m->evt_m3 - m->tail_m3;
            m->events.last_volume_nm3 = m->evt_nm3 - m->tail_nm3;
            m->events.last_duration_s = m->evt_s - m->tail_s;
            m->evt_active = false;
            m->above_s = 0.0;
            m->cand_m3 = m->cand_nm3 = m->cand_s = 0.0;
            m->depl_s = 0.0;
            m->depl_latched = false;
            evt_bins_clear(m);
        }
    } else {
        m->below_s = 0.0;
        m->tail_m3 = m->tail_nm3 = m->tail_s = 0.0;
    }
}

/* ---------- 고갈 (6절) ---------- */

static double idle_pressure(const meter_t *m)
{
    /* P_idle = 직전 idle_window_min 분 중 흐름 없던 분의 정압 중앙값 */
    double v[METER_IDLE_SLOTS];
    uint16_t n = 0;
    for (uint16_t k = 0; k < m->cfg.idle_window_min; k++) {
        uint16_t i = (uint16_t)((m->idle_head + METER_IDLE_SLOTS - 1u - k) % METER_IDLE_SLOTS);
        if (m->idle_ok[i]) {
            v[n++] = m->idle_pg[i];
        }
    }
    return meter_median(v, n);
}

static void depl_step(meter_t *m, double pg, double q_h, double dt)
{
    const meter_cfg_t *c = &m->cfg;
    if (!m->evt_active) {
        return;
    }
    const double p_idle = idle_pressure(m);
    bool cond = false;
    if (!isnan(p_idle) && p_idle > 0.0 && pg < c->depl_p_ratio * p_idle && m->evt_nbins > 0) {
        /* 이벤트 중앙값은 30 s 칸 평균들의 중앙값 */
        double bins[METER_EVT_BINS];
        for (uint16_t k = 0; k < m->evt_nbins; k++) {
            bins[k] = m->evt_bins[(m->evt_bin_head + METER_EVT_BINS - 1u - k) % METER_EVT_BINS];
        }
        const double med = meter_median(bins, m->evt_nbins);
        cond = q_h < c->depl_flow_ratio * med;
    }
    if (cond) {
        m->depl_s += dt;
        if (m->depl_s >= c->depl_hold_s && !m->depl_latched) {
            m->depletions++;
            m->depl_latched = true;     /* 조건이 풀릴 때까지 한 번만 셈 */
        }
    } else {
        m->depl_s = 0.0;
        m->depl_latched = false;
    }
}

/* ---------- 매 측정 ---------- */

void meter_step(meter_t *m, const meter_sample_t *s, double dt)
{
    const meter_cfg_t *c = &m->cfg;
    if (dt <= 0.0) {
        return;
    }
    /* HSC 영점 따라가기 — SDP810 이 있고 흐름이 없을 때만 (meter_cfg_t 설명). 이번 값부터 새 영점으로 계산 */
    if (c->zero_track_tau_s > 0.0 && s->dp2_ok && !m->evt_active && s->dp2_pa > -c->zero_track_band_pa &&
        s->dp2_pa < c->zero_track_band_pa) {
        const double diff = (s->dp_raw_pa - m->zero_pa) - s->dp2_pa;
        if (diff > -c->zero_track_diff_pa && diff < c->zero_track_diff_pa) {
            const double a = dt / c->zero_track_tau_s;
            double z = m->zero_pa + diff * (a > 1.0 ? 1.0 : a);
            const double lo = m->zero_base_pa - c->zero_track_max_pa, hi = m->zero_base_pa + c->zero_track_max_pa;
            if (z < lo || z > hi) {
                z = z < lo ? lo : hi;       /* 한도 — 관이 빠졌거나 센서 한쪽이 이상함 */
                m->zero_limit_hits++;
            }
            m->zero_pa = z;
            m->zero_track_s += dt;
        }
    }
    const double dp = s->dp_raw_pa - m->zero_pa;           /* 5절: ΔP = ΔP_raw − Z */
    const double t_k = s->t_gas_c + METER_T_STD_K;
    const double p_abs = s->patm_pa + s->pg_pa;
    const double rho = meter_gas_density(p_abs, t_k, c->gas_molar_kg_mol);
    const double cd = meter_discharge_c(c, dp, rho, t_k, NULL);
    const double q = meter_flow_m3s(dp, rho, cd, c->throat_d_m, c->inlet_d_m);
    const double nf = meter_normal_factor(p_abs, t_k);
    const double q_h = q * 3600.0;

    m->last_rho = rho;
    m->last_t_k = t_k;
    m->last_flow_m3h = q_h;
    m->last_flow_nm3h = q_h * nf;

    /* 4절: 사용량 = Σ Qₙ × Δt — 컷오프 아래는 잡음으로 보고 0 */
    const double dv = (q_h < c->cutoff_m3h) ? 0.0 : q * dt;
    const double dvn = dv * nf;
    m->total_m3 += dv;
    m->total_nm3 += dvn;

    evt_step(m, q_h, dv, dvn, dt);
    depl_step(m, s->pg_pa, q_h, dt);

    /* 1분 누적 (시간 가중 평균) */
    m->m_s += dt;
    m->m_dp += dp * dt;
    m->m_pg += s->pg_pa * dt;
    m->m_t += s->t_gas_c * dt;
    m->m_patm += s->patm_pa * dt;
    m->m_m3 += dv;
    m->m_nm3 += dvn;
    if (m->evt_active) {            /* 분 단위 '흐름 있음' = 사용 이벤트 중 (잡음으로 잠깐 튄 건 안 침) */
        m->m_flowing = true;
    }
    if (m->depl_latched) {
        m->m_depl = true;
    }
    if (fabs(s->dp_raw_pa) > 0.95 * c->dp_full_scale_pa || s->pg_pa > 0.95 * c->pg_full_scale_pa) {
        m->m_oor = true;
    }

    /* 8절: 누설 B 는 30분 평균 차압 — 차압을 먼저 평균, 유량은 나중 */
    m->lw_dp_acc += dp * dt;
    m->lw_s += dt;
}

/* ---------- 1분마다 ---------- */

void meter_minute(meter_t *m, meter_minute_t *out)
{
    const meter_cfg_t *c = &m->cfg;
    meter_minute_t r;
    memset(&r, 0, sizeof(r));
    r.minute = m->minute_index;
    r.secs = m->m_s;
    if (m->m_s > 0.0) {
        r.dp_mean_pa = m->m_dp / m->m_s;
        r.pg_mean_pa = m->m_pg / m->m_s;
        r.t_mean_c = m->m_t / m->m_s;
        r.patm_mean_pa = m->m_patm / m->m_s;
        r.flow_m3h = m->m_m3 / m->m_s * 3600.0;
        r.flow_nm3h = m->m_nm3 / m->m_s * 3600.0;
    }
    r.flowing = m->m_flowing;
    r.out_of_range = m->m_oor;
    r.depleted = m->m_depl;

    /* 6절: P_idle 용 — 흐름 없던 분의 정압만 */
    m->idle_pg[m->idle_head] = r.pg_mean_pa;
    m->idle_ok[m->idle_head] = (m->m_s > 0.0) && !r.flowing;
    m->idle_head = (uint16_t)((m->idle_head + 1u) % METER_IDLE_SLOTS);

    /* 7절: 방출 분 — ① 정압 ≥ P_vent − δ  ② 10분 전과 변화 5 Pa 미만  ③ 유량 0
     * (④ 1초 압력 흔들림은 보조 조건이라 아직 안 넣음 — 센서 잡음을 벤치에서 잰 뒤) */
    if (m->m_s > 0.0) {
        bool venting = false;
        if (c->vent_p_pa > 0.0 && m->flat_n >= c->vent_flat_min) {
            const uint16_t ago = (uint16_t)((m->flat_head + METER_FLAT_SLOTS - c->vent_flat_min) % METER_FLAT_SLOTS);
            const double delta = fmax(c->vent_delta_min_pa, c->vent_delta_frac * c->vent_p_pa);
            const bool high = r.pg_mean_pa >= c->vent_p_pa - delta;
            const bool flat = fabs(r.pg_mean_pa - m->flat_pg[ago]) < c->vent_flat_pa;
            const bool no_flow = !r.flowing && r.flow_m3h < c->event_end_m3h;
            venting = high && flat && no_flow;
        }
        r.venting = venting;
        if (venting) {
            m->vent_minutes++;
        }
        /* flat_pg[head] = 지금 분, head 를 하나 밀면 vent_flat_min 칸 뒤가 10분 전 */
        m->flat_pg[m->flat_head] = r.pg_mean_pa;
        m->flat_head = (uint16_t)((m->flat_head + 1u) % METER_FLAT_SLOTS);
        if (m->flat_n < METER_FLAT_SLOTS) {
            m->flat_n++;
        }
    }

    /* 8절: 누설 B — 조리가 없는 30분 구간의 평균 차압이 문턱을 넘는 구간이 4개 이어지면 판정.
     * 조리가 끼인 구간은 판정하지 않고 연속을 끊음(밤 누설을 보는 용도라 보수적으로). */
    if (r.flowing) {
        m->lw_valid = false;
    }
    m->lw_minutes++;
    if (m->lw_minutes >= c->leak_window_min) {
        if (m->lw_valid && m->lw_s > 0.0) {
            const double dp_avg = m->lw_dp_acc / m->lw_s;
            if (dp_avg >= c->leak_dp_min_pa) {
                m->leak_streak++;
                const double c_leak = c->c_re_a > 0.0
                                          ? meter_discharge_c(c, dp_avg, m->last_rho, m->last_t_k, NULL)
                                          : c->discharge_c_low;
                m->leak_flow_m3h = 3600.0 * meter_flow_m3s(dp_avg, m->last_rho, c_leak, c->throat_d_m, c->inlet_d_m);
            } else {
                m->leak_streak = 0;
            }
        } else {
            m->leak_streak = 0;
        }
        const bool was_leak = m->leak_active;
        m->leak_active = m->leak_streak >= c->leak_windows;
        if (m->leak_active && !was_leak) {
            m->leaks++;
        }
        if (!m->leak_active && m->leak_streak == 0) {
            m->leak_flow_m3h = 0.0;
        }
        m->lw_dp_acc = 0.0;
        m->lw_s = 0.0;
        m->lw_minutes = 0;
        m->lw_valid = true;
    }

    /* 다음 분 준비 */
    m->minute_index++;
    m->m_s = m->m_dp = m->m_pg = m->m_t = m->m_patm = m->m_m3 = m->m_nm3 = 0.0;
    m->m_flowing = m->evt_active;
    m->m_oor = false;
    m->m_depl = m->depl_latched;

    if (out != NULL) {
        *out = r;
    }
}
