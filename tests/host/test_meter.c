/* 계산 모듈 맥 시험 — 계산식 문서의 예제 값과 대조
 * 실행: ./tests/host/run.sh
 */
#include <math.h>
#include <stdio.h>
#include "meter.h"

static int fails = 0, checks = 0;

static void near(const char *what, double got, double want, double tol)
{
    checks++;
    const bool ok = fabs(got - want) <= tol;
    if (!ok) {
        fails++;
    }
    printf("  %s %-46s %12.6g  (기대 %g ± %g)\n", ok ? "ok  " : "FAIL", what, got, want, tol);
}

static void is_true(const char *what, bool v)
{
    checks++;
    if (!v) {
        fails++;
    }
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

/* 예제 1 · 2 조건: Kisii 고도 1,700 m · 정압 300 Pa · 22 °C · dₜ 4.62 / D 25 mm · C 0.97 */
static meter_sample_t ex_sample(double dp, double pg)
{
    meter_sample_t s = {dp, pg, 22.0, meter_patm_from_altitude(1700.0), false};
    return s;
}

/* 예제 조건에서 원하는 실제 유량(m³/h)을 내는 차압 */
static double dp_for_flow(const meter_cfg_t *c, double q_m3h, double pg)
{
    const meter_sample_t s = ex_sample(1.0, pg);
    const double rho = meter_gas_density(s.patm_pa + pg, 22.0 + METER_T_STD_K, c->gas_molar_kg_mol);
    const double q1 = 3600.0 * meter_flow_m3s(1.0, rho, c->discharge_c, c->throat_d_m, c->inlet_d_m);
    return (q_m3h / q1) * (q_m3h / q1);     /* Q ∝ √ΔP */
}

static void run(meter_t *m, double dp, double pg, double secs, double dt, uint32_t *sec_clock)
{
    const meter_sample_t s = ex_sample(dp, pg);
    for (double t = 0.0; t < secs - 1e-9; t += dt) {
        meter_step(m, &s, dt);
        *sec_clock += (uint32_t)dt;
        if (*sec_clock % 60u == 0u) {
            meter_minute(m, NULL);
        }
    }
}

static void test_flow_examples(void)
{
    printf("예제 1 · 2: 유량과 표준상태 (2 · 3절)\n");
    meter_cfg_t c;
    meter_default_cfg(&c);
    near("평균 분자량 kg/mol", c.gas_molar_kg_mol, 0.02723, 0.00001);

    const meter_sample_t s = ex_sample(32.4, 300.0);
    const double t_k = 22.0 + METER_T_STD_K;
    const double p_abs = s.patm_pa + s.pg_pa;
    const double rho = meter_gas_density(p_abs, t_k, c.gas_molar_kg_mol);
    const double area = M_PI * c.throat_d_m * c.throat_d_m / 4.0;
    const double beta = c.throat_d_m / c.inlet_d_m;
    near("③ 목 넓이 Aₜ m²", area, 0.00001676, 0.00000001);
    near("④ 속력 √(2ΔP/ρ) m/s", sqrt(2.0 * 32.4 / rho), 8.40, 0.01);
    near("⑤ √(1 − β⁴)", sqrt(1.0 - pow(beta, 4)), 0.9994, 0.0001);

    const double q = meter_flow_m3s(32.4, rho, c.discharge_c, c.throat_d_m, c.inlet_d_m);
    near("① 유량 m³/s", q, 0.000137, 0.0000005);
    near("유량 m³/h (예제 1: 0.49)", q * 3600.0, 0.49, 0.005);

    const double nf = meter_normal_factor(p_abs, t_k);
    near("기압 비율 (Kisii 0.817)", p_abs / METER_P_STD_PA, 0.817, 0.002);
    near("온도 비율 (22 °C 0.926)", METER_T_STD_K / t_k, 0.926, 0.001);
    near("표준 유량 Nm³/h (예제 2: 0.37)", q * 3600.0 * nf, 0.37, 0.005);
    near("표준상태로 줄어드는 비율 (약 24 %)", 1.0 - nf, 0.24, 0.01);

    printf("영점 오차가 유량에 주는 영향 (5절 표)\n");
    const double e32 = meter_flow_m3s(32.6, rho, 1, 1e-3, 1) / meter_flow_m3s(32.4, rho, 1, 1e-3, 1) - 1.0;
    const double e16 = meter_flow_m3s(1.8, rho, 1, 1e-3, 1) / meter_flow_m3s(1.6, rho, 1, 1e-3, 1) - 1.0;
    near("32.4 Pa 에 +0.2 Pa → +0.3 %", e32 * 100.0, 0.3, 0.05);
    near("1.6 Pa 에 +0.2 Pa → +6.1 %", e16 * 100.0, 6.1, 0.05);

    printf("영점 Z 빼기 (ΔP = ΔP_raw − Z)\n");
    meter_t m;
    meter_init(&m, &c);
    meter_set_zero(&m, 0.2);
    meter_sample_t raw = ex_sample(32.6, 300.0);
    meter_step(&m, &raw, 1.0);
    near("영점 0.2 를 빼면 예제 1 유량", m.last_flow_m3h, q * 3600.0, 1e-9);
}

static void test_event_example3(void)
{
    printf("예제 3: 68분 · 0.45 m³/h 조리 → 0.51 m³ (4절)\n");
    meter_cfg_t c;
    meter_default_cfg(&c);
    meter_t m;
    meter_init(&m, &c);
    uint32_t clk = 0;
    const double dp = dp_for_flow(&c, 0.45, 300.0);
    run(&m, 0.0, 300.0, 120, 10, &clk);         /* 평소: 10 s 마다 */
    run(&m, dp, 300.0, 68 * 60, 1, &clk);        /* 조리: 1 s 마다 */
    run(&m, 0.0, 300.0, 300, 1, &clk);
    is_true("이벤트 1회", m.events.count == 1);
    near("이벤트 부피 m³ (0.45 × 68 ÷ 60)", m.events.last_volume_m3, 0.51, 0.001);
    near("이벤트 시간 s (68분)", m.events.last_duration_s, 68 * 60, 1.0);
    near("누적 사용량 m³", m.total_m3, 0.51, 0.001);
    is_true("표준 부피가 실제보다 작음(고도 · 온도)", m.events.last_volume_nm3 < m.events.last_volume_m3);
    is_true("조리 끝난 뒤 흐름 없음", !meter_is_flowing(&m));

    printf("짧은 흐름(20 s)은 이벤트가 아님 (시작은 30 s 이상)\n");
    meter_init(&m, &c);
    clk = 0;
    run(&m, dp, 300.0, 20, 1, &clk);
    run(&m, 0.0, 300.0, 120, 1, &clk);
    is_true("이벤트 0회", m.events.count == 0 && !m.evt_active);
    near("그래도 사용량은 더함", m.total_m3, 0.45 * 20 / 3600.0, 1e-6);

    printf("측정 간격이 1 s 든 10 s 든 같은 사용량\n");
    meter_t a, b;
    meter_init(&a, &c);
    meter_init(&b, &c);
    uint32_t ca = 0, cb = 0;
    run(&a, dp, 300.0, 600, 1, &ca);
    run(&b, dp, 300.0, 600, 10, &cb);
    near("1 s 와 10 s 의 누적 차이 m³", a.total_m3 - b.total_m3, 0.0, 1e-12);
}

static void test_depletion_example4(void)
{
    printf("예제 4: 고갈 — P_idle 250 Pa, 조리 중 60 Pa · 0.15 m³/h 로 2분 (6절)\n");
    meter_cfg_t c;
    meter_default_cfg(&c);
    meter_t m;
    meter_init(&m, &c);
    uint32_t clk = 0;
    run(&m, 0.0, 250.0, 125 * 60, 10, &clk);                 /* 흐름 없는 2시간 → P_idle 250 */
    run(&m, dp_for_flow(&c, 0.45, 250.0), 250.0, 10 * 60, 1, &clk);
    run(&m, dp_for_flow(&c, 0.15, 60.0), 60.0, 120, 1, &clk);
    is_true("고갈 1회", m.depletions == 1);

    printf("60 s 안 되면 고갈 아님\n");
    meter_init(&m, &c);
    clk = 0;
    run(&m, 0.0, 250.0, 125 * 60, 10, &clk);
    run(&m, dp_for_flow(&c, 0.45, 250.0), 250.0, 10 * 60, 1, &clk);
    run(&m, dp_for_flow(&c, 0.15, 60.0), 60.0, 50, 1, &clk);
    run(&m, dp_for_flow(&c, 0.45, 250.0), 250.0, 60, 1, &clk);
    is_true("고갈 0회", m.depletions == 0);

    printf("압력만 떨어지고 유량은 그대로면 고갈 아님\n");
    meter_init(&m, &c);
    clk = 0;
    run(&m, 0.0, 250.0, 125 * 60, 10, &clk);
    run(&m, dp_for_flow(&c, 0.45, 250.0), 250.0, 10 * 60, 1, &clk);
    run(&m, dp_for_flow(&c, 0.45, 60.0), 60.0, 120, 1, &clk);
    is_true("고갈 0회", m.depletions == 0);
}

static void test_vent(void)
{
    printf("방출 시간: 물트랩 20 cm → P_vent 1,962 Pa (7절)\n");
    meter_cfg_t c;
    meter_default_cfg(&c);
    near("P_vent Pa", c.vent_p_pa, 1962.0, 0.01);
    meter_t m;
    meter_init(&m, &c);
    uint32_t clk = 0;
    run(&m, 0.0, 1960.0, 20 * 60, 10, &clk);        /* 20분 동안 1,960 Pa 에 붙어 있음 */
    is_true("10분 비교가 쌓인 뒤 11분째부터 셈 → 10분", m.vent_minutes == 10);

    printf("압력이 계속 오르는 중이면 방출 아님(평탄 조건)\n");
    meter_init(&m, &c);
    clk = 0;
    for (int k = 0; k < 20; k++) {
        run(&m, 0.0, 1880.0 + 10.0 * k, 60, 10, &clk);
    }
    is_true("방출 0분", m.vent_minutes == 0);

    printf("방출 높이보다 한참 낮으면 방출 아님\n");
    meter_init(&m, &c);
    clk = 0;
    run(&m, 0.0, 1500.0, 20 * 60, 10, &clk);
    is_true("방출 0분", m.vent_minutes == 0);
}

static void test_leak_example6(void)
{
    printf("예제 6: 누설 B — 30분 평균 차압 0.4 Pa 가 2시간 → 약 55 L/h (8절)\n");
    meter_cfg_t c;
    meter_default_cfg(&c);
    meter_t m;
    meter_init(&m, &c);
    uint32_t clk = 0;
    /* 1초 값은 잡음으로 −0.1 / +0.9 Pa 를 오가고 평균만 0.4 Pa */
    for (int k = 0; k < 3 * 30 * 60; k++) {
        const meter_sample_t s = ex_sample((k & 1) ? 0.9 : -0.1, 300.0);
        meter_step(&m, &s, 1.0);
        if (++clk % 60u == 0u) {
            meter_minute(&m, NULL);
        }
    }
    is_true("90분(구간 3개)으로는 아직 아님", !m.leak_active);
    for (int k = 0; k < 30 * 60; k++) {
        const meter_sample_t s = ex_sample((k & 1) ? 0.9 : -0.1, 300.0);
        meter_step(&m, &s, 1.0);
        if (++clk % 60u == 0u) {
            meter_minute(&m, NULL);
        }
    }
    is_true("120분(구간 4개)이면 누설 판정", m.leak_active);
    near("누설 유량 m³/h (약 55 L/h)", m.leak_flow_m3h, 0.0544, 0.002);
    is_true("잡음으로는 사용 이벤트가 안 생김", m.events.count == 0 && !m.evt_active);

    printf("중간에 조리가 끼면 연속이 끊김\n");
    meter_init(&m, &c);
    clk = 0;
    run(&m, 0.4, 300.0, 60 * 60, 1, &clk);
    run(&m, dp_for_flow(&c, 0.45, 300.0), 300.0, 5 * 60, 1, &clk);
    run(&m, 0.4, 300.0, 85 * 60, 1, &clk);
    is_true("누설 아님(연속 4구간이 안 됨)", !m.leak_active);
}

static void test_minute_summary(void)
{
    printf("1분 요약\n");
    meter_cfg_t c;
    meter_default_cfg(&c);
    meter_t m;
    meter_init(&m, &c);
    const meter_sample_t s = ex_sample(32.4, 300.0);
    for (int k = 0; k < 60; k++) {
        meter_step(&m, &s, 1.0);
    }
    meter_minute_t r;
    meter_minute(&m, &r);
    near("1분 평균 유량 m³/h", r.flow_m3h, 0.49, 0.005);
    near("1분 평균 표준 유량 Nm³/h", r.flow_nm3h, 0.37, 0.005);
    near("평균 차압 Pa", r.dp_mean_pa, 32.4, 1e-9);
    near("측정 시간 s", r.secs, 60.0, 1e-9);
    is_true("조리 중으로 표시", r.flowing);
}

static void test_cutoff(void)
{
    printf("잡음 컷오프 (11/17 결정: 0.05 m³/h 아래는 사용량에 안 더함)\n");
    meter_cfg_t c;
    meter_default_cfg(&c);
    meter_t m;
    meter_init(&m, &c);
    /* 영점 근처 잡음: 0 · +1칸 · +2칸(0.306 Pa) 을 하루 동안 번갈아 */
    const double noise[3] = {0.0, 0.153, 0.306};
    for (int k = 0; k < 86400; k++) {
        const meter_sample_t s = ex_sample(noise[k % 3], 300.0);
        meter_step(&m, &s, 1.0);
        if ((k + 1) % 60 == 0) {
            meter_minute(&m, NULL);
        }
    }
    near("잡음 하루 → 사용량 m³", m.total_m3, 0.0, 1e-12);
    is_true("잡음으로 이벤트 안 생김", m.events.count == 0u && !m.evt_active);

    meter_init(&m, &c);
    uint32_t clk = 0;
    run(&m, dp_for_flow(&c, 0.06, 300.0), 300.0, 3600.0, 1.0, &clk);
    near("0.06 m³/h 한 시간 → 0.06 m³ (그대로 셈)", m.total_m3, 0.06, 0.0005);

    meter_init(&m, &c);
    clk = 0;
    const double dp_lo = dp_for_flow(&c, 0.048, 300.0);
    run(&m, dp_lo, 300.0, 3.0 * 3600.0, 1.0, &clk);
    near("0.048 m³/h 세 시간 → 0 (컷오프 아래)", m.total_m3, 0.0, 1e-12);
    is_true("그래도 누설 B 는 자르기 전 차압으로 잡힘", dp_lo >= c.leak_dp_min_pa && m.leak_active);
    printf("       (0.048 m³/h = 차압 %.3f Pa, 누설 문턱 %.2f Pa)\n", dp_lo, c.leak_dp_min_pa);
}

int main(void)
{
    test_flow_examples();
    test_event_example3();
    test_depletion_example4();
    test_vent();
    test_leak_example6();
    test_minute_summary();
    test_cutoff();
    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
