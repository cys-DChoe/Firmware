/* 계량기 계산 모듈 — 하드웨어와 무관한 순수 계산 (맥에서도 시험함: tests/host)
 *
 * 근거: "바이오가스 계량기 계산식" https://claude.ai/artifact/MAQopiVVYw6BqANQ25Xx32
 *   2절 유량(벤투리) · 3절 표준상태 · 4절 적산 · 사용 이벤트 · 5절 영점
 *   6절 고갈 · 7절 방출 시간 · 8절 누설 B(계량기 뒤)
 * 설계서 3번 결정(나눠서): 방출량 · 계량기 앞 누설(A) · 이용률(BUF)은 서버가 계산 → 여기 없음.
 *
 * 단위: 압력 Pa, 온도 °C(입력) · K(내부), 지름 m, 유량 m³/h, 부피 m³ · Nm³, 시간 s
 * 판정 문턱은 계산식 문서의 "벤치 시험 전 제안값"이라 전부 설정값으로 둠.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define METER_P_STD_PA      101325.0   /* 표준 기압 */
#define METER_T_STD_K       273.15     /* 0 °C */
#define METER_R_J_MOL_K     8.314      /* 기체상수 */

typedef struct {
    /* 벤투리 (설계서 2번: 설정값, 지금은 계산식 문서 예제 값) */
    double throat_d_m;          /* 목 지름 dₜ */
    double inlet_d_m;           /* 입구 지름 D */
    double discharge_c;         /* 유출계수 C (보통 유량) */
    double discharge_c_low;     /* 저유량용 C (누설 B 유량 계산) */
    double gas_molar_kg_mol;    /* 평균 분자량: CH₄ 60 % · CO₂ 40 % → 0.02723 */

    /* 사용 이벤트 (4절) — 실제 유량 기준 */
    double event_start_m3h;     /* 0.05 m³/h 넘음 */
    double event_start_hold_s;  /* 30 s 이상 */
    double event_end_m3h;       /* 0.03 m³/h 아래 */
    double event_end_hold_s;    /* 60 s 이상 */
    /* 잡음 컷오프 (11/17 결정): 이보다 작은 유량은 사용량에 안 더함.
     * 차압이 작으면 √ 때문에 크게 부풀려짐 — HSC 1칸(0.153 Pa)만 떠도 0.03 m³/h, 하루 0.7 m³.
     * 기본값 = 이벤트 시작 기준 0.05 m³/h (≈ 0.45 Pa). 벤치 잡음 재고 바꿈.
     * 이벤트 · 고갈 판정과 누설 B(30분 평균 차압)는 자르기 전 값으로 봄. */
    double cutoff_m3h;

    /* 고갈 (6절) */
    double depl_p_ratio;        /* P_g < 0.3 × P_idle */
    double depl_flow_ratio;     /* 유량 < 이벤트 중앙값 × 0.5 */
    double depl_hold_s;         /* 60 s 이상 */
    uint16_t idle_window_min;   /* P_idle = 직전 120분 중 흐름 없던 때 정압 중앙값 */

    /* 방출 시간 (7절) */
    double vent_p_pa;           /* P_vent = 98.1 × 물트랩 높이 cm (설치 입력) */
    double vent_delta_min_pa;   /* δ = max(50 Pa, 5 % × P_vent) */
    double vent_delta_frac;
    double vent_flat_pa;        /* 10분 전과 비교해 변화 5 Pa 미만 */
    uint16_t vent_flat_min;     /* 10분 */

    /* 누설 B (8절) */
    uint16_t leak_window_min;   /* 30분 구간 */
    uint16_t leak_windows;      /* 4개(2시간) 연속 */
    double leak_dp_min_pa;      /* "잡음보다 뚜렷하게 큰" 30분 평균 차압 — 벤치에서 정함 */

    /* 데이터 품질 (10절): 센서 만점의 95 % 를 넘으면 범위 이탈 */
    double dp_full_scale_pa;    /* HSC ±10 mbar → 1000 Pa */
    double pg_full_scale_pa;    /* ABP2 정압 만점 */
} meter_cfg_t;

/* 한 번 읽은 센서 값 */
typedef struct {
    double dp_raw_pa;   /* 차압 센서 값 (영점 빼기 전) */
    double pg_pa;       /* 정압(게이지압) */
    double t_gas_c;     /* 가스 온도 */
    double patm_pa;     /* 대기압 (BMP390) */
    bool patm_held;     /* 기압을 못 재서 마지막 값(한 번도 못 쟀으면 해수면 값)을 씀 — 계산은 이 값으로 함 */
    /* 비교 시험용 (다음 판 · 계산에는 안 씀, 1분 보고 24 · 26 번에만): */
    bool dp2_ok;        /* SDP810 값이 있음 */
    double dp2_pa;      /* SDP810 차압 (영점 안 뺌) */
    bool t_probe_ok;    /* DS18B20 값이 있음 (그때 t_gas_c 도 이 값) */
    double t_probe_c;   /* DS18B20 온도 */
} meter_sample_t;

/* 1분 요약 — 저장 · 보고에 씀 */
typedef struct {
    uint32_t minute;            /* 시작 뒤 몇 번째 분 */
    double secs;                /* 이 분에 들어온 측정 시간 합 (완전성) */
    double dp_mean_pa;          /* 평균 차압 (영점 보정 후) */
    double pg_mean_pa;
    double t_mean_c;
    double patm_mean_pa;
    double flow_m3h;            /* 1분 평균 실제 유량 = 이 분 부피 ÷ 시간 */
    double flow_nm3h;           /* 1분 평균 표준 유량 */
    bool flowing;               /* 이 분에 흐름(이벤트)이 있었나 */
    bool venting;               /* 방출 중인 분 */
    bool depleted;              /* 이 분에 고갈 상태였음 (6절 판정이 선 동안) */
    bool out_of_range;          /* 차압 · 정압 만점 95 % 넘음 */
} meter_minute_t;

typedef struct {
    uint32_t count;             /* 끝난 이벤트 수 */
    double last_volume_nm3;     /* 마지막 이벤트 부피 */
    double last_volume_m3;
    double last_duration_s;
} meter_events_t;

#define METER_IDLE_SLOTS   180   /* P_idle 창 최대 3시간(분) */
#define METER_FLAT_SLOTS   16    /* 방출 평탄 비교 최대 15분 전 */
#define METER_EVT_BINS     480   /* 이벤트 중앙값용 30 s 칸, 최대 4시간 */
#define METER_EVT_BIN_S    30.0

typedef struct {
    meter_cfg_t cfg;
    double zero_pa;             /* 영점 Z (설치 때 버튼으로) */

    /* 적산 — 정전 뒤에도 이어 가야 해서 바깥에서 저장 · 복원함 */
    double total_nm3;           /* 누적 사용량 (표준) */
    double total_m3;            /* 누적 사용량 (실제 부피) */

    /* 지금 값 */
    double last_flow_m3h;
    double last_flow_nm3h;
    double last_rho;

    /* 사용 이벤트 */
    bool evt_active;
    double above_s, below_s;    /* 문턱 넘은 · 밑돈 연속 시간 */
    double cand_nm3, cand_m3, cand_s;   /* 시작 확정 전 쌓는 값 */
    double tail_nm3, tail_m3, tail_s;   /* 끝 확정 전 꼬리(종료 시각 이후 몫) */
    double evt_nm3, evt_m3, evt_s;
    double evt_bins[METER_EVT_BINS];
    uint16_t evt_nbins;
    uint16_t evt_bin_head;
    double bin_acc_flow, bin_acc_s;
    meter_events_t events;

    /* 고갈 */
    double idle_pg[METER_IDLE_SLOTS];
    bool idle_ok[METER_IDLE_SLOTS];
    uint16_t idle_head;
    double depl_s;
    bool depl_latched;
    uint32_t depletions;        /* 고갈 횟수 */

    /* 방출 */
    double flat_pg[METER_FLAT_SLOTS];
    uint16_t flat_n, flat_head;
    uint32_t vent_minutes;      /* 방출 분 수 (누적) */

    /* 누설 B */
    double lw_dp_acc, lw_s;
    uint16_t lw_minutes;
    bool lw_valid;
    uint16_t leak_streak;
    bool leak_active;
    uint32_t leaks;             /* 누설(계량기 뒤) 판정이 꺼짐 → 켜짐으로 바뀐 횟수 (누적, 11/18) */
    double leak_flow_m3h;       /* 마지막 30분 평균 차압으로 구한 누설 유량 */

    /* 이 분 누적 */
    uint32_t minute_index;
    double m_s, m_dp, m_pg, m_t, m_patm, m_m3, m_nm3;
    bool m_flowing, m_oor, m_depl;
} meter_t;

/* 계산식 문서 예제 값으로 채운 기본 설정 */
void meter_default_cfg(meter_cfg_t *cfg);
void meter_init(meter_t *m, const meter_cfg_t *cfg);

/* 센서를 한 번 읽을 때마다 (조리 중 1 s, 평소 10 s — dt_s 에 실제 간격) */
void meter_step(meter_t *m, const meter_sample_t *s, double dt_s);

/* 1분마다: 1분 요약을 만들고 방출 · P_idle · 누설 B 를 갱신 */
void meter_minute(meter_t *m, meter_minute_t *out);

/* 영점: 버튼 길게 눌렀을 때, 그동안 읽은 차압 평균을 Z 로 */
void meter_set_zero(meter_t *m, double zero_pa);

/* 흐름이 있나 — 측정 주기(1 s / 10 s)를 정할 때 씀 */
bool meter_is_flowing(const meter_t *m);

/* 순수 식 (시험 · 다른 모듈에서도 씀) */
double meter_gas_density(double p_abs_pa, double t_k, double molar_kg_mol);
double meter_flow_m3s(double dp_pa, double rho, double c, double throat_d_m, double inlet_d_m);
double meter_normal_factor(double p_abs_pa, double t_k);
double meter_patm_from_altitude(double alt_m);   /* BMP390 이 없을 때 · 시험용 */
double meter_median(const double *v, uint16_t n);
