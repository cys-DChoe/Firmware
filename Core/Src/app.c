#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "lowpower.h"

static void say(app_t *a, const char *m)
{
    if (a->hw.log != NULL) {
        a->hw.log(a->hw.ctx, m);
    }
}

static void flash_on(app_t *a, bool on)
{
    if (a->hw.flash_power != NULL) {
        a->hw.flash_power(a->hw.ctx, on);
    }
}

static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[b[i] >> 4];
        out[2 * i + 1] = H[b[i] & 15];
    }
    out[2 * n] = '\0';
}

void app_init(app_t *a, const app_hw_t *hw, ring_t *ring, nvlog_t *nv_set, nvlog_t *nv_ctr, rak_t *rak)
{
    memset(a, 0, sizeof(*a));
    a->hw = *hw;
    a->ring = ring;
    a->nv_set = nv_set;
    a->nv_ctr = nv_ctr;
    a->rak = rak;

    meter_cfg_t cfg;
    meter_default_cfg(&cfg);
    flash_on(a, true);
    (void)settings_load(nv_set, &a->settings);          /* 없으면 기본값 */
    if (a->settings.vent_p_pa > 0.0) {
        cfg.vent_p_pa = a->settings.vent_p_pa;
    }
    cfg.pg_full_scale_pa = (double)app_pg_fs_pa(a);
    meter_init(&a->meter, &cfg);
    if (a->hw.set_pg_fs != NULL) {
        a->hw.set_pg_fs(a->hw.ctx, (double)app_pg_fs_pa(a));   /* 센서 쪽 환산 · 자르기 범위도 같이 */
    }
    if (a->hw.set_sdp != NULL) {
        a->hw.set_sdp(a->hw.ctx, !a->settings.sdp_off);
    }
    if (a->settings.sdp_off) {
        say(a, "SDP810: 설정으로 꺼 둠 (04 01 로 켬)");
    }
    if (a->settings.zero_set) {
        meter_set_zero(&a->meter, a->settings.zero_pa);
    }

    if (counter_load(nv_ctr, &a->counter) == NVLOG_OK) {
        a->meter.total_nm3 = a->counter.total_nm3;
        a->meter.total_m3 = a->counter.total_m3;
        a->meter.vent_minutes = a->counter.vent_minutes;
        a->meter.depletions = a->counter.depletions;
        a->meter.events.count = a->counter.events;
        a->meter.leaks = a->counter.leaks;
        if (a->counter.leak_active) {
            /* 누설 판정 중에 다시 켜짐: 판정을 이어 감 (안 그러면 2시간 뒤 다시 켜지며 한 번 더 셈) */
            a->meter.leak_active = true;
            a->meter.leak_streak = a->meter.cfg.leak_windows;
        }
        /* 1분마다 링에 먼저 쓰고 누적을 나중에 저장함 → 그 사이 전원이 끊기면 링이 한 분 앞섬.
         * 링 마지막 보고가 누적이 아직 안 센 순번이면:
         *   순번은 늘 그 다음으로 (안 그러면 다음 보고가 같은 순번으로 나감),
         *   누적은 큰 쪽으로 (보고는 0.1 L 로 반올림돼 있어, 그 분에 안 썼으면 저장값이 더 클 수 있음) */
        ring_rec_t last;
        report_t r;
        if (ring_last(ring, &last) == RING_OK && report_decode(last.report, RING_REPORT_SIZE, &r) &&
            r.seq == a->counter.report_seq) {
            a->counter.report_seq = (uint16_t)(r.seq + 1u);
            if (r.total_nm3 > a->counter.total_nm3) {
                const double add = r.total_nm3 - a->counter.total_nm3;
                const double ratio = a->counter.total_nm3 > 0.0 ? a->counter.total_m3 / a->counter.total_nm3 : 1.0;
                a->meter.total_nm3 = r.total_nm3;
                a->meter.total_m3 += add * ratio;       /* 실제 부피는 보고에 없어 비율로 */
            }
            say(a, "누적: 링이 한 분 앞서 있어 순번을 이음");
        }
    } else {
        /* 누적 기록이 없으면 링의 마지막 1분 보고로 되살림 (0.1 L 단위) */
        ring_rec_t last;
        report_t r;
        if (ring_last(ring, &last) == RING_OK && report_decode(last.report, RING_REPORT_SIZE, &r)) {
            a->meter.total_nm3 = r.total_nm3;
            a->counter.report_seq = (uint16_t)(r.seq + 1u);
            say(a, "누적: 링 마지막 보고에서 되살림");
        }
    }
    flash_on(a, false);
    a->boot_flags = REPORT_F_BOOT;
}

double app_vent_p_pa(const app_t *a)
{
    if (a->settings.vent_p_pa > 0.0) {
        return a->settings.vent_p_pa;
    }
    meter_cfg_t d;
    meter_default_cfg(&d);
    return d.vent_p_pa;
}

uint16_t app_pg_fs_pa(const app_t *a)
{
    return a->settings.pg_fs_pa != 0u ? a->settings.pg_fs_pa : (uint16_t)APP_PG_FS_DEFAULT_PA;
}

/* ---------- 교정값 (11/24 검수 2번) ---------- */

/* 펌웨어 기본값 (meter_default_cfg) — cal_t 단위로 */
static double cal_default(unsigned k)
{
    meter_cfg_t d;
    meter_default_cfg(&d);
    switch (k) {
    case CAL_C_RE_A: return d.c_re_a;
    case CAL_C_RE_B: return d.c_re_b;
    case CAL_C_RE_MIN: return d.c_re_min;
    case CAL_THROAT_MM: return d.throat_d_m * 1e3;
    case CAL_INLET_MM: return d.inlet_d_m * 1e3;
    case CAL_VISC_UPAS: return d.visc_20c_pa_s * 1e6;
    case CAL_VISC_S_K: return d.visc_sutherland_k;
    default: return d.gas_molar_kg_mol * 1e3;
    }
}

static double cal_get(const cal_t *c, unsigned k)
{
    return (c->has & (1u << k)) ? (double)c->v[k] : cal_default(k);
}

double app_cal_value(const app_t *a, unsigned k)
{
    return cal_get(&a->cal, k);
}

/* 교정값을 계산 설정에 넣음. 안 넣은 칸은 기본값 그대로(double 정밀도) */
static void cal_to_cfg(const cal_t *c, meter_cfg_t *cfg)
{
    cfg->c_re_a = cal_get(c, CAL_C_RE_A);
    cfg->c_re_b = cal_get(c, CAL_C_RE_B);
    cfg->c_re_min = cal_get(c, CAL_C_RE_MIN);
    cfg->throat_d_m = cal_get(c, CAL_THROAT_MM) * 1e-3;
    cfg->inlet_d_m = cal_get(c, CAL_INLET_MM) * 1e-3;
    cfg->visc_20c_pa_s = cal_get(c, CAL_VISC_UPAS) * 1e-6;
    cfg->visc_sutherland_k = cal_get(c, CAL_VISC_S_K);
    cfg->gas_molar_kg_mol = cal_get(c, CAL_MOLAR_GMOL) * 1e-3;
}

/* 칸마다 범위 + 넣고 나서 식 전체가 말이 되는지 (β, Re 최소와 아주 큰 Re 에서 C 가 0.5–1.05) */
static bool cal_ok(const cal_t *c)
{
    static const double lo[CAL_N] = {0.5, -5000.0, 100.0, 1.0, 3.0, 5.0, 20.0, 2.0};
    static const double hi[CAL_N] = {1.2, 5000.0, 100000.0, 30.0, 100.0, 40.0, 600.0, 100.0};
    for (unsigned k = 0; k < CAL_N; k++) {
        const double v = cal_get(c, k);
        if (!isfinite(v)) {
            return false;
        }
        if (k == CAL_C_RE_A && v == 0.0) {
            continue;                       /* a 0 = C(Re) 끔 */
        }
        if (v < lo[k] || v > hi[k]) {
            return false;
        }
    }
    const double beta = cal_get(c, CAL_THROAT_MM) / cal_get(c, CAL_INLET_MM);
    if (beta < 0.05 || beta > 0.8) {
        return false;
    }
    const double a = cal_get(c, CAL_C_RE_A), b = cal_get(c, CAL_C_RE_B), re_min = cal_get(c, CAL_C_RE_MIN);
    if (a > 0.0) {
        const double c_lo = a + b / re_min, c_hi = a + b / 1e6;
        if (c_lo < METER_C_MIN || c_lo > METER_C_MAX || c_hi < METER_C_MIN || c_hi > METER_C_MAX) {
            return false;
        }
    }
    return true;
}

size_t app_cal_encode(const app_t *a, uint8_t o[APP_CAL_LEN])
{
    o[0] = 1u;
    o[1] = a->cal.has;
    for (unsigned k = 0; k < CAL_N; k++) {
        const float f = (float)app_cal_value(a, k);
        uint32_t u;
        memcpy(&u, &f, 4);
        o[2 + 4 * k] = (uint8_t)(u >> 24);
        o[3 + 4 * k] = (uint8_t)(u >> 16);
        o[4 + 4 * k] = (uint8_t)(u >> 8);
        o[5 + 4 * k] = (uint8_t)u;
    }
    return APP_CAL_LEN;
}

void app_set_cal(app_t *a, nvlog_t *nv_cal)
{
    a->nv_cal = nv_cal;
    memset(&a->cal, 0, sizeof(a->cal));
    if (nv_cal != NULL) {
        flash_on(a, true);
        (void)cal_load(nv_cal, &a->cal);    /* 없으면 has 0 = 모두 기본값 */
        flash_on(a, false);
    }
    if (!cal_ok(&a->cal)) {
        memset(&a->cal, 0, sizeof(a->cal));  /* 저장값이 깨졌으면 기본값으로 (계산이 엉뚱해지지 않게) */
        say(a, "교정값: 저장값이 범위 밖 — 기본값으로");
    }
    cal_to_cfg(&a->cal, &a->meter.cfg);
    if (a->cal.has != 0u) {
        char m[64];
        snprintf(m, sizeof(m), "교정값: 넣은 칸 %02X (나머지는 기본값)", (unsigned)a->cal.has);
        say(a, m);
    }
}

/* 넣은 칸 표시와 넣은 칸의 값만 비교 (구조체 틈(padding)은 보지 않음 — 검수 2차) */
static bool cal_same(const cal_t *x, const cal_t *y)
{
    if (x->has != y->has) {
        return false;
    }
    for (unsigned k = 0; k < CAL_N; k++) {
        if ((x->has & (1u << k)) && x->v[k] != y->v[k]) {
            return false;
        }
    }
    return true;
}

static bool cal_command(app_t *a, const uint8_t *d, uint8_t len)
{
    if (a->nv_cal == NULL) {
        say(a, "설정: 교정값 저장 자리가 없음 — 무시");
        return false;
    }
    cal_t c = a->cal;
    if (len == 2u && d[1] == 0xFFu) {
        a->cal_report_pending = true;       /* 보고만 */
        return true;
    }
    if (len == 2u && d[1] == 0x00u) {
        memset(&c, 0, sizeof(c));
    } else if (len >= 6u && (len - 1u) % 5u == 0u && (len - 1u) / 5u <= CAL_N) {
        /* 칸을 다 넣고 나서 한 번만 검사 · 적용 — 같이 바뀌는 a · b · Re 최소가 섞인 채로 계산되지 않게 */
        for (uint8_t i = 1u; i < len; i += 5u) {
            const uint8_t kk = d[i];
            const uint8_t k = (uint8_t)(kk & 0x7Fu);
            if (k < 1u || k > CAL_N) {
                say(a, "설정: 교정값 칸 번호가 틀림 — 무시");
                return false;
            }
            const uint8_t bit = (uint8_t)(1u << (k - 1u));
            if (kk & 0x80u) {
                c.has &= (uint8_t)~bit;      /* 이 칸만 기본값으로 */
                c.v[k - 1u] = 0.0f;
                continue;
            }
            const uint32_t u = (uint32_t)d[i + 1u] << 24 | (uint32_t)d[i + 2u] << 16 | (uint32_t)d[i + 3u] << 8 | d[i + 4u];
            float f;
            memcpy(&f, &u, 4);
            c.v[k - 1u] = f;
            c.has |= bit;
        }
    } else {
        say(a, "설정: 교정값 명령 모양이 틀림 — 무시");
        return false;
    }
    if (!cal_ok(&c)) {
        say(a, "설정: 교정값 범위 밖 · C 가 0.5–1.05 를 벗어남 — 명령 전체 무시");
        return false;
    }
    a->cal_report_pending = true;           /* 바뀌었든 아니든 지금 값을 올려 서버가 확인 */
    if (cal_same(&c, &a->cal)) {
        say(a, "설정: 교정값 같은 값 — 그대로");
        return true;
    }
    a->cal = c;
    flash_on(a, true);
    const bool ok = cal_save(a->nv_cal, &a->cal) == NVLOG_OK;
    flash_on(a, false);
    cal_to_cfg(&a->cal, &a->meter.cfg);
    say(a, ok ? "설정: 교정값 바꿈" : "설정: 교정값 바꿈 (저장 실패 — 다시 켜면 옛 값)");
    return true;
}

bool app_downlink(app_t *a, uint8_t port, const uint8_t *d, uint8_t len)
{
    if (port != APP_CFG_PORT || len < 1u) {
        return false;
    }
    if (d[0] == APP_CFG_PG_FS && len == 3u) {
        const uint16_t pa = (uint16_t)((d[1] << 8) | d[2]);
        if (pa != 0u && (pa < APP_PG_FS_MIN_PA || pa > APP_PG_FS_MAX_PA)) {
            say(a, "설정: 정압 만점 범위 밖 — 무시");
            return false;
        }
        if (pa == a->settings.pg_fs_pa) {
            say(a, "설정: 정압 만점 같은 값 — 그대로");   /* 서버가 되풀이해 보내도 저장 칸을 닳게 하지 않음 */
            return true;
        }
        a->settings.pg_fs_pa = pa;
        flash_on(a, true);
        const bool ok = settings_save(a->nv_set, &a->settings) == NVLOG_OK;
        flash_on(a, false);
        a->meter.cfg.pg_full_scale_pa = (double)app_pg_fs_pa(a);
        if (a->hw.set_pg_fs != NULL) {
            a->hw.set_pg_fs(a->hw.ctx, (double)app_pg_fs_pa(a));
        }
        say(a, ok ? "설정: 정압 만점 바꿈" : "설정: 정압 만점 바꿈 (저장 실패 — 다시 켜면 옛 값)");
        return true;
    }
    if (d[0] == APP_CFG_VENT_P && len == 3u) {
        const uint16_t pa = (uint16_t)((d[1] << 8) | d[2]);
        if (pa != 0u && (pa < APP_VENT_P_MIN_PA || pa > app_pg_fs_pa(a))) {
            say(a, "설정: 방출 기준 범위 밖 — 무시");
            return false;
        }
        if ((double)pa == a->settings.vent_p_pa) {
            say(a, "설정: 방출 기준 같은 값 — 그대로");
            return true;
        }
        a->settings.vent_p_pa = (double)pa;
        flash_on(a, true);
        const bool ok = settings_save(a->nv_set, &a->settings) == NVLOG_OK;
        flash_on(a, false);
        a->meter.cfg.vent_p_pa = app_vent_p_pa(a);
        say(a, ok ? "설정: 방출 기준 바꿈" : "설정: 방출 기준 바꿈 (저장 실패 — 다시 켜면 옛 값)");
        return true;
    }
    if (d[0] == APP_CFG_SDP && len == 2u) {
        if (d[1] > 1u) {
            say(a, "설정: SDP810 값은 00 · 01 만 — 무시");
            return false;
        }
        const bool off = d[1] == 0u;
        if (off == a->settings.sdp_off) {
            say(a, "설정: SDP810 같은 값 — 그대로");
            return true;
        }
        a->settings.sdp_off = off;
        flash_on(a, true);
        const bool ok = settings_save(a->nv_set, &a->settings) == NVLOG_OK;
        flash_on(a, false);
        if (a->hw.set_sdp != NULL) {
            a->hw.set_sdp(a->hw.ctx, !off);
        }
        say(a, ok ? (off ? "설정: SDP810 끔" : "설정: SDP810 켬")
                  : (off ? "설정: SDP810 끔 (저장 실패 — 다시 켜면 옛 값)" : "설정: SDP810 켬 (저장 실패 — 다시 켜면 옛 값)"));
        return true;
    }
    if (d[0] == APP_CFG_CAL) {
        return cal_command(a, d, len);
    }
    if (d[0] == APP_CFG_LTE_TEST && len == 1u) {
        (void)app_lte_test(a);
        return true;
    }
    say(a, "설정: 모르는 명령");
    return false;
}

void app_set_lte(app_t *a, lte_t *lte)
{
    a->lte = lte;
}

bool app_lte_test(app_t *a)
{
    if (a->lte == NULL || lte_busy(a->lte) || a->lte_want || a->batt_off) {
        return false;
    }
    const uint16_t mv = a->hw.batt_mv != NULL ? a->hw.batt_mv(a->hw.ctx) : 0u;
    if (mv < APP_LTE_MIN_MV) {
        a->lte_refused++;
        say(a, "LTE: 셀이 3.65 V 아래 — 시험 안 함");
        memset(&a->lte_last, 0, sizeof(a->lte_last));
        a->lte_last.code = LTE_R_LOW_BATT;  /* 서버가 '셀 낮아 안 함'을 알게 결과로 올림 */
        a->lte_last.csq = 99;
        a->lte_report_pending = true;
        return false;
    }
    say(a, "LTE: 켜기 시작");
    a->lte_want = true;                     /* lte_work 가 그때의 up_s 로 시작 (여기서 부르면 up_s 가 한 걸음 낡았을 수 있음) */
    return true;
}

/* LTE 시험을 한 칸 진행. 끝났으면 결과를 남기고 LoRa 로 올릴 차례로.
 * 시간은 up_s(안 뛰는 초)로 — RTC 시각을 쓰면 시계 맞출 때 기다림이 한꺼번에 끝남 */
static void lte_work(app_t *a, bool report)
{
    if (a->lte == NULL) {
        return;
    }
    const uint32_t now = a->up_s;
    if (a->lte_want && !lte_busy(a->lte)) {
        a->lte_want = false;
        (void)lte_start(a->lte, now);
    }
    if (!lte_busy(a->lte) && !a->lte->done && (int32_t)(now - a->next_lte_guard) >= 0) {
        /* 켤 때 한 번 + 10분마다: 시키지 않았는데 켜져 있으면 끔
         * (워치독 · 오류로 MCU 만 다시 켜진 뒤, 또는 지난번에 못 껐을 때 — 켜진 채면 12.5 mA 가 계속 흐름) */
        a->next_lte_guard = now + APP_LTE_GUARD_S;
        if (lte_off_start(a->lte, now)) {
            a->lte_left_on++;
            say(a, "LTE: 켜져 있음 → 끔");
        }
    }
    if (lte_busy(a->lte)) {
        (void)lte_step(a->lte, now);
    }
    if (a->lte->done) {
        a->lte->done = false;
        a->lte_last = a->lte->res;
        a->lte_report_pending = report;
        char m[96];
        snprintf(m, sizeof(m), "LTE: %s · 신호 %u · %s · %u s%s", lte_code_name(a->lte_last.code),
                 (unsigned)a->lte_last.csq, a->lte_last.oper, (unsigned)a->lte_last.secs,
                 a->lte_last.off == LTE_OFF_FAILED ? " · 못 끔!" : "");
        say(a, m);
    }
}

void app_set_boot(app_t *a, bool abnormal)
{
    a->boot_flags = REPORT_F_BOOT | (abnormal ? REPORT_F_FAULT_RESET : 0u);
}

static bool no_answer(rak_err_t e) { return e == RAK_E_TIMEOUT || e == RAK_E_IO; }

/* 가입 시작 (시각 · 횟수 기록) */
static rak_err_t join(app_t *a, uint32_t now)
{
    const rak_err_t e = rak_join_start(a->rak);
    a->joins++;
    a->join_start_s = now;
    a->next_join_s = now + APP_JOIN_RETRY_S;
    if (no_answer(e)) {
        a->lora_misses++;
    }
    return e;
}

/* 설정 실패 · 무응답이면 10분 뒤 모듈 리셋부터 다시 */
static rak_err_t lora_start_failed(app_t *a, rak_err_t e, const char *msg)
{
    say(a, msg);
    a->lora_ready = false;
    a->lora_need_reset = true;
    a->next_lora_try_s = a->hw.now_s(a->hw.ctx) + APP_JOIN_RETRY_S;
    return e;
}

rak_err_t app_lora_start(app_t *a)
{
    a->lora_started = true;
    a->lora_ready = false;
    a->tx_live = false;
    a->tx_is_lte = false;                   /* 보내던 LTE 결과도 버림 — 남아 있으면 다음 링 기록이 '보냄' 표시를 못 받음 */
    a->tx_is_cnt = false;
    a->tx_is_cal = false;
    rak_give_up(a->rak);                    /* 처음부터 다시: 전에 기다리던 소식은 버림 */
    /* 켤 때 셀이 이미 낮으면 가입(송신)부터 하지 않음 — 1분 보고 한 번으로 바로 쉬러 가게 */
    const uint16_t mv0 = a->hw.batt_mv != NULL ? a->hw.batt_mv(a->hw.ctx) : 0u;
    if (mv0 != 0u && mv0 < APP_BATT_OFF_MV) {
        a->batt_low_n = (uint8_t)(APP_BATT_OFF_COUNT - 1u);
        a->next_lora_try_s = a->hw.now_s(a->hw.ctx) + APP_JOIN_RETRY_S;
        say(a, "LoRa: 셀이 낮아 시작 안 함");
        return RAK_E_BUSY;
    }
    bool set = false;
    rak_err_t e = rak_ensure_baud(a->rak, APP_LORA_BAUD, APP_LORA_BAUD_OLD, &set);
    if (e != RAK_OK) {
        return lora_start_failed(a, e, "LoRa: 모듈 응답 없음");
    }
    if (!a->settings.keys_set) {
        say(a, "LoRa: 키 없음 — 기록만 쌓음");
        return RAK_OK;
    }
    char dev[17], app[17], key[33];
    hex(a->settings.dev_eui, 8, dev);
    hex(a->settings.app_eui, 8, app);
    hex(a->settings.app_key, 16, key);
    rak_cfg_t c;
    rak_default_cfg(&c);
    c.band = a->settings.band;
    c.lbt = (c.band == 7u);          /* 국내 KR920 만 LBT */
    c.dev_eui = dev;
    c.app_eui = app;
    c.app_key = key;
    e = rak_configure(a->rak, &c);
    if (e != RAK_OK) {
        return lora_start_failed(a, e, "LoRa: 설정 실패");
    }
    a->lora_ready = true;
    a->lora_need_reset = false;
    a->lora_misses = 0;
    return join(a, a->hw.now_s(a->hw.ctx));
}

static void build_report(app_t *a, const meter_minute_t *mm, uint32_t now, report_t *r)
{
    memset(r, 0, sizeof(*r));
    r->seq = a->counter.report_seq;
    r->unix_s = now;
    r->total_nm3 = a->meter.total_nm3;
    r->flow_nm3h = mm->flow_nm3h;
    r->pg_pa = mm->pg_mean_pa;
    r->dp_pa = mm->dp_mean_pa;
    r->t_c = mm->t_mean_c;
    r->patm_pa = mm->patm_mean_pa;
    uint16_t f = 0;
    if (mm->flowing) f |= REPORT_F_FLOWING;
    if (mm->venting) f |= REPORT_F_VENTING;
    if (mm->depleted) f |= REPORT_F_DEPLETED;
    if (a->meter.leak_active) f |= REPORT_F_LEAK_B;
    if (mm->out_of_range) f |= REPORT_F_OUT_OF_RANGE;
    if (!a->settings.zero_set) f |= REPORT_F_ZERO_UNSET;
    if (a->hw.time_valid == NULL || !a->hw.time_valid(a->hw.ctx)) f |= REPORT_F_TIME_UNSYNCED;
    if (mm->secs < 54.0) f |= REPORT_F_INCOMPLETE;
    if (a->baro_held_min) f |= REPORT_F_BARO_HELD;
    a->baro_held_min = false;
    /* 비교 시험용 두 칸: 이 분에 읽은 값의 평균, 없으면 NAN → 0x8000 */
    r->sdp_dp_pa = a->sdp_n > 0u ? a->sdp_sum / (double)a->sdp_n : NAN;
    r->t_probe_c = a->probe_n > 0u ? a->probe_sum / (double)a->probe_n : NAN;
    a->sdp_sum = a->probe_sum = 0.0;
    a->sdp_n = a->probe_n = 0u;
    const uint16_t mv = a->hw.batt_mv != NULL ? a->hw.batt_mv(a->hw.ctx) : 0u;
    if (mv != 0u) {
        a->batt_flags = report_battery_flags(mv, a->batt_flags);
        f |= a->batt_flags;
        /* 3.3 V 종료: 한 번 튄 값으로 멈추지 않게 이어서 낮을 때만 */
        a->batt_low_n = (mv < APP_BATT_OFF_MV) ? (uint8_t)(a->batt_low_n + 1u) : 0u;
        if (a->batt_low_n >= APP_BATT_OFF_COUNT) {
            f |= REPORT_F_BATT_OFF;
            a->batt_off_pending = true;
        }
    }
    f |= a->boot_flags;                 /* 켠 뒤 첫 기록에만 */
    a->boot_flags = 0u;
    r->batt_mv = mv;
    r->flags = f;
}

static void do_minute(app_t *a, uint32_t now)
{
    meter_minute_t mm;
    meter_minute(&a->meter, &mm);
    report_t r;
    build_report(a, &mm, now, &r);
    {
        /* 보드에서 값을 바로 보게 1분마다 한 줄 (보드 오면 2 · 4 · 7단계) */
        char m[320];                    /* 보통 220 B, 값이 다 커도 270 B 안 (11/24 SDP810 · DS18B20 · 영점 칸 더함) */
        snprintf(m, sizeof(m),
                 "1분 #%u: 차압 %.2f Pa · 정압 %.0f Pa · 기압 %.0f Pa · 온도 %.1f C · 셀 %u mV · 유량 %.3f Nm3/h · 누적 %.4f Nm3 · 비트 %04X · SDP810 %.2f Pa · DS18B20 %.2f C · 영점 %.2f Pa",
                 (unsigned)r.seq, r.dp_pa, r.pg_pa, r.patm_pa, r.t_c, (unsigned)r.batt_mv, r.flow_nm3h, r.total_nm3,
                 (unsigned)r.flags, r.sdp_dp_pa, r.t_probe_c, a->meter.zero_pa);
        say(a, m);
    }
    if (a->meter.zero_limit_hits != a->zero_hits_said) {
        a->zero_hits_said = a->meter.zero_limit_hits;
        say(a, "영점 따라가기: 버튼 영점 ±1 Pa 한도에 닿음 — SDP810 관이 빠졌거나 센서 한쪽 이상, 관 확인");
    }
    uint8_t bytes[REPORT_LEN];
    report_encode(&r, bytes);

    flash_on(a, true);
    uint32_t seq;
    bool stored = true;
    if (ring_append(a->ring, bytes, &seq) == RING_OK) {
        a->fresh_pending = true;
        a->fresh_seq = seq;
    } else {
        stored = false;
        say(a, "링 저장 실패");
    }
    a->counter.total_nm3 = a->meter.total_nm3;
    a->counter.total_m3 = a->meter.total_m3;
    a->counter.ring_next_seq = a->ring->next_seq;
    a->counter.report_seq = (uint16_t)(r.seq + 1u);
    a->counter.vent_minutes = a->meter.vent_minutes;
    a->counter.depletions = a->meter.depletions;
    a->counter.events = a->meter.events.count;
    a->counter.leaks = a->meter.leaks;
    a->counter.leak_active = a->meter.leak_active;
    a->counter.unix_s = now;
    if (counter_save(a->nv_ctr, &a->counter) != NVLOG_OK) {
        stored = false;
        say(a, "누적 저장 실패");
    }
    a->store_failed = !stored;          /* LED 빨강 3번 */
    flash_on(a, false);
    a->reports++;
}

/* 링의 기록 하나를 보냄. resend 면 "다시 보냄" 비트를 켜서. */
static bool send_seq(app_t *a, uint32_t seq, bool resend)
{
    ring_rec_t rec;
    flash_on(a, true);
    const ring_err_t e = ring_read(a->ring, seq, &rec);
    flash_on(a, false);
    if (e != RING_OK) {
        return false;
    }
    uint8_t out[REPORT_LEN];
    memcpy(out, rec.report, REPORT_LEN);
    if (resend) {
        report_t r;
        if (report_decode(out, REPORT_LEN, &r)) {
            r.flags |= REPORT_F_RESENT;
            report_encode(&r, out);
        }
    }
    const rak_err_t se = rak_send_start(a->rak, APP_LORA_PORT, out, REPORT_LEN);
    if (se != RAK_OK) {
        if (no_answer(se)) {
            a->lora_misses++;
        }
        return false;
    }
    a->tx_live = true;
    a->tx_is_lte = false;
    a->tx_is_cnt = false;
    a->tx_is_cal = false;
    a->tx_seq = seq;
    a->tx_start_s = a->hw.now_s(a->hw.ctx);
    return true;
}

/* 망 시각으로 RTC 맞추기. 맞추면 true (그때 now 가 바뀜) */
static void time_sync(app_t *a, uint32_t now)
{
    if (a->hw.set_time == NULL || (a->time_synced && (int32_t)(now - a->next_timesync_s) < 0)) {
        return;
    }
    if (!a->timereq_on) {
        if (rak_time_request(a->rak, true) == RAK_OK) {
            a->timereq_on = true;           /* 다음 송신에 실려 나감 */
        }
        return;
    }
    uint32_t t;
    if (rak_get_time(a->rak, &t) != RAK_OK) {
        return;                             /* 아직 답이 안 옴 — 다음 송신 뒤 다시 */
    }
    const int32_t diff = (int32_t)(t - now);
    if ((diff > APP_TIME_STEP_S || diff < -APP_TIME_STEP_S) && a->hw.set_time(a->hw.ctx, t)) {
        /* 시계가 뛴 만큼 기준도 옮겨서 큰 간격을 적산하지 않게.
         * RTC 초로 잰 기다림(송신 · 가입 소식 · 다시 보냄 · 다시 가입 · LED · 쉬는 중 셀 재기)도 같이 옮김 —
         * 안 옮기면 앞으로 뛸 땐 한꺼번에 끝나고(방금 시작한 송신이 '소식 없음'), 뒤로 뛸 땐 그만큼 멈춤 (11/18 검수) */
        const uint32_t d = (uint32_t)diff;
        a->last_s += d;
        a->last_minute = t / 60u;
        a->tx_start_s += d;
        a->join_start_s += d;
        a->next_resend_s += d;
        a->next_join_s += d;
        a->next_lora_try_s += d;
        a->next_led_s += d;
        a->off_since += d;
        a->next_batt_check_s += d;
        a->cnt_last_s += d;
        a->next_cnt_try_s += d;
        char m[48];
        snprintf(m, sizeof(m), "시계 맞춤: %ld 초", (long)diff);
        say(a, m);
    }
    a->time_synced = true;
    a->time_syncs++;
    a->next_timesync_s = t + APP_TIMESYNC_S;
    if (rak_time_request(a->rak, false) == RAK_OK) {
        a->timereq_on = false;              /* 매 송신마다 물어 보지 않게 */
    }
}

/* 지금 횟수 (누적) */
static void counts_now(const app_t *a, report_counts_t *c)
{
    c->seq = (uint16_t)(a->counter.report_seq - 1u);   /* 마지막으로 만든 1분 보고 */
    c->cooking = a->meter.events.count;
    c->depletions = a->meter.depletions;
    c->vent_minutes = a->meter.vent_minutes;
    c->leaks = a->meter.leaks;
}

/* 횟수 보고를 보낼 때인가: 켠 뒤 처음 · 바뀜 · 하루 지남 — 그리고 10분 간격 */
static bool counts_due(const app_t *a, uint32_t now)
{
    if ((int32_t)(now - a->next_cnt_try_s) < 0) {
        return false;
    }
    if (a->reports == 0u && a->counter.report_seq == 0u) {
        return false;                       /* 새 계량기: 1분 보고를 하나 만든 뒤부터 (보고 번호 칸이 65535 로 나가지 않게) */
    }
    if (!a->cnt_sent_valid || (int32_t)(now - a->cnt_last_s) >= (int32_t)APP_CNT_DAY_S) {
        return true;
    }
    report_counts_t c;
    counts_now(a, &c);
    /* 방출 분은 방출 중 매분 늘어남 → 그것만으로는 안 보냄 (방출 여부는 1분 보고 비트로 앎). 다른 것과 같이 · 하루 한 번 실림 */
    return c.cooking != a->cnt_sent.cooking || c.depletions != a->cnt_sent.depletions || c.leaks != a->cnt_sent.leaks;
}

static void lora_work(app_t *a, uint32_t now)
{
    if (a->rak == NULL) {
        return;
    }
    if (!a->lora_ready) {
        /* 켤 때 · 리셋 뒤 설정이 안 됐으면 10분마다 다시 (키가 없으면 기록만 쌓음) */
        if (a->lora_started && a->settings.keys_set && (int32_t)(now - a->next_lora_try_s) >= 0) {
            if (a->lora_need_reset) {
                a->lora_restarts++;
                (void)rak_hw_reset(a->rak);
            }
            (void)app_lora_start(a);
        }
        return;
    }
    rak_poll(a->rak);
    if (a->rak->rx_new) {
        a->rak->rx_new = false;
        (void)app_downlink(a, a->rak->rx.port, a->rak->rx.data, a->rak->rx.len);
    }
    if (a->rak->joined != a->joined_seen) {
        a->joined_seen = a->rak->joined;
        if (a->joined_seen) {
            a->lora_misses = 0;             /* 가입 소식이 왔으니 모듈은 살아 있음 */
        }
    }

    /* 기다리던 소식이 안 옴 → 놓친 것으로 보고 풂 (모듈이 줄 하나를 빠뜨리거나 멈춤) */
    if (a->tx_live && a->rak->tx_pending && (int32_t)(now - a->tx_start_s) >= (int32_t)APP_TX_TIMEOUT_S) {
        rak_give_up(a->rak);                /* 아래에서 실패로 세고 링에 "안 보냄"으로 남음 */
        a->tx_timeouts++;
        a->lora_misses++;
        say(a, "LoRa: 송신 끝 소식 없음");
    }
    if (a->rak->joining && (int32_t)(now - a->join_start_s) >= (int32_t)APP_JOIN_TIMEOUT_S) {
        rak_give_up(a->rak);                /* 10분 간격 다시 가입으로 넘어감 */
        a->lora_misses++;
        say(a, "LoRa: 가입 결과 소식 없음");
    }
    if (a->lora_misses >= APP_LORA_MISS_MAX) {
        say(a, "LoRa: 연달아 무응답 → 모듈 리셋");
        a->lora_ready = false;
        a->lora_need_reset = true;
        a->timereq_on = false;              /* 리셋 뒤 시각 요청도 다시 */
        a->next_lora_try_s = now;
        a->lora_misses = 0;
        return;
    }

    /* 보내던 것이 끝났나 */
    if (a->tx_live && !a->rak->tx_pending) {
        a->tx_live = false;
        const bool was_lte = a->tx_is_lte;
        const bool was_cnt = a->tx_is_cnt;
        const bool was_cal = a->tx_is_cal;
        a->tx_is_lte = false;
        a->tx_is_cnt = false;
        a->tx_is_cal = false;
        if (a->rak->last_tx == RAK_EV_TX_DONE || a->rak->last_tx == RAK_EV_CONFIRMED_OK) {
            if (was_cnt) {
                a->cnt_sent = a->cnt_sending;
                a->cnt_sent_valid = true;
                a->cnt_last_s = now;
                a->cnt_reports++;
            }
            if (!was_lte && !was_cnt && !was_cal) {
                flash_on(a, true);
                ring_mark_sent(a->ring, a->tx_seq);
                flash_on(a, false);
            }
            a->sent++;
            a->lora_misses = 0;
            a->tx_failed = false;
            time_sync(a, now);
            now = a->hw.now_s(a->hw.ctx);   /* 시계를 맞췄으면 아래 송신 시각이 낡지 않게 */
        } else {
            a->send_fail++;          /* 링에 "안 보냄"으로 남아 나중에 다시 보냄 */
            a->tx_failed = true;
        }
    }

    if (!a->rak->joined) {
        if (!a->rak->joining && (int32_t)(now - a->next_join_s) >= 0) {
            (void)join(a, now);
        }
        return;
    }
    if (!a->timereq_on && !a->time_synced && a->hw.set_time != NULL) {
        time_sync(a, now);                  /* 가입 직후 첫 송신부터 시각을 물음 */
        now = a->hw.now_s(a->hw.ctx);
    }
    if (a->tx_live || a->rak->tx_pending) {
        return;
    }
    if (a->fresh_pending) {
        a->fresh_pending = false;           /* 실패해도 링에 남아 다시 보내기로 감 */
        send_seq(a, a->fresh_seq, false);
        a->next_resend_s = now + APP_RESEND_GAP_S;
        return;
    }
    if (a->batt_off) {
        return;                             /* 쉬는 중엔 마지막 보고만 — 밀린 기록 · LTE 결과는 다시 켠 뒤 (11/18 검수) */
    }
    if (a->lte_report_pending) {
        uint8_t b[7];
        const size_t n = lte_result_encode(&a->lte_last, b);
        const rak_err_t se = rak_send_start(a->rak, APP_LTE_PORT, b, n);
        if (se == RAK_OK) {
            a->lte_report_pending = false;
            a->tx_live = true;
            a->tx_is_lte = true;
            a->tx_is_cnt = false;
            a->tx_is_cal = false;
            a->tx_start_s = now;
        } else if (no_answer(se)) {
            a->lora_misses++;
        }
        return;
    }
    if (a->cal_report_pending) {
        uint8_t b[APP_CAL_LEN];
        const size_t n = app_cal_encode(a, b);
        const rak_err_t se = rak_send_start(a->rak, APP_CAL_PORT, b, n);
        if (se == RAK_OK) {
            a->cal_report_pending = false;
            a->tx_live = true;
            a->tx_is_cal = true;
            a->tx_is_lte = false;
            a->tx_is_cnt = false;
            a->tx_start_s = now;
        } else if (no_answer(se)) {
            a->lora_misses++;
        }
        return;
    }
    if (counts_due(a, now)) {
        a->next_cnt_try_s = now + APP_CNT_GAP_S;   /* 실패해도 10분 뒤에 다시 */
        counts_now(a, &a->cnt_sending);
        uint8_t b[REPORT_COUNTS_LEN];
        const size_t n = report_counts_encode(&a->cnt_sending, b);
        const rak_err_t se = rak_send_start(a->rak, APP_CNT_PORT, b, n);
        if (se == RAK_OK) {
            a->tx_live = true;
            a->tx_is_cnt = true;
            a->tx_is_lte = false;
            a->tx_is_cal = false;
            a->tx_start_s = now;
        } else if (no_answer(se)) {
            a->lora_misses++;
        }
        return;
    }
    if ((int32_t)(now - a->next_resend_s) >= 0) {
        uint32_t seq;
        flash_on(a, true);
        const ring_err_t e = ring_find_unsent(a->ring, APP_RESEND_WINDOW, &seq);
        flash_on(a, false);
        if (e == RING_OK && send_seq(a, seq, true)) {
            a->resends++;
        }
        a->next_resend_s = now + APP_RESEND_GAP_S;
    }
}

static void power_save(app_t *a, bool off)
{
    if (a->hw.power_save != NULL) {
        a->hw.power_save(a->hw.ctx, off);
    }
}

/* 셀이 낮음 → 측정 · 보고를 멈추고 쉼 (누적은 방금 1분 저장에서 이미 저장됨) */
static void batt_off_enter(app_t *a, uint32_t now)
{
    a->batt_off_pending = false;
    a->batt_off = true;
    a->batt_offs++;
    a->off_since = now;
    a->next_batt_check_s = now + APP_OFF_CHECK_S;
    a->lte_want = false;
    if (a->lte != NULL) {
        lte_stop(a->lte);                   /* 켜져 있으면 먼저 끔 (펌웨어 주의 2절) — 쉬는 동안에도 끝까지 진행 */
    }
    power_save(a, true);
    say(a, "전원: 셀 낮음 → 측정 · 보고 멈춤");
}

static void batt_off_leave(app_t *a, uint32_t now, uint16_t mv)
{
    a->batt_off = false;
    a->batt_low_n = 0;
    power_save(a, false);
    a->prev_ok = false;                 /* 쉬기 전 값으로 적산하지 않게 */
    a->dp_n = 0;                        /* 영점도 쉬기 전 차압으로 잡지 않게 */
    a->dp_head = 0;
    a->last_s = now;
    a->last_minute = now / 60u;         /* 첫 분은 반쪽이라 다음 분 경계부터 보고 */
    char m[48];
    snprintf(m, sizeof(m), "전원: 셀 %u mV → 다시 시작", (unsigned)mv);
    say(a, m);
}

/* 쉬는 동안: 마지막 보고만 마저 보내고, 10분마다 셀을 잼 */
static uint32_t batt_off_step(app_t *a, uint32_t now)
{
    uint32_t dt = now - a->last_s;
    a->last_s = now;
    a->up_s += ((int32_t)dt < 0) ? 0u : (dt > APP_MAX_DT_S ? APP_MAX_DT_S : dt);
    if (a->lte != NULL) {
        lte_work(a, false);
        if (lte_busy(a->lte)) {
            return 1000u;                   /* LTE 를 다 끌 때까지는 1 s 마다 */
        }
    }
    if ((int32_t)(now - a->off_since) < (int32_t)APP_OFF_TX_GRACE_S && a->rak != NULL && a->lora_ready &&
        a->rak->joined && (a->fresh_pending || a->tx_live)) {
        lora_work(a, now);
    }
    if ((int32_t)(now - a->next_batt_check_s) >= 0) {
        a->next_batt_check_s = now + APP_OFF_CHECK_S;
        const uint16_t mv = a->hw.batt_mv != NULL ? a->hw.batt_mv(a->hw.ctx) : 0u;
        if (mv >= APP_BATT_ON_MV) {
            batt_off_leave(a, now, mv);
            return lp_tick_ms(false);
        }
    }
    return APP_OFF_TICK_MS;
}

/* 10 s 마다 상태를 LED 로 한 번 보임 */
static void led_tick(app_t *a, uint32_t now)
{
    if ((int32_t)(now - a->next_led_s) < 0) {
        return;
    }
    a->next_led_s = now + APP_LED_PERIOD_S;
    const bool lora_bad = !a->settings.keys_set || a->rak == NULL || !a->lora_ready || !a->rak->joined || a->tx_failed;
    a->led_state = a->store_failed                 ? APP_LED_STORE
                   : (!a->read_ok || a->baro_held) ? APP_LED_SENSOR
                   : lora_bad                      ? APP_LED_LORA
                                                   : APP_LED_OK;
    if (a->hw.led != NULL) {
        if (a->led_state == APP_LED_OK) {
            a->hw.led(a->hw.ctx, false, 1u);
        } else {
            a->hw.led(a->hw.ctx, true, a->led_state);
        }
    }
}

/* DS18B20 이 빠지거나(150 s 넘게 값 없음 → 가스 온도가 BMP390 으로) 다시 잡히면 로그 한 줄 (설계서 PCB3_3 5번).
 * 켤 때는 main 이 있음 · 없음을 찍으므로, 켠 뒤 처음 정해지는 상태는 조용히 받아 둠.
 * 첫 값은 변환(750 ms) 뒤 다음 읽기에서 나오므로 APP_PROBE_GRACE_S 까지는 "없음"으로 안 봄.
 * PCB3_2(자리 없음)는 늘 없음 → 로그 안 나옴. */
static void probe_watch(app_t *a, bool ok)
{
    if (ok) {
        if (a->probe_state == 2u) {
            say(a, "DS18B20: 다시 잡힘 — 가스 온도를 프로브 값으로 (보고 26번 다시 채움)");
        }
        a->probe_state = 1u;
    } else if (a->probe_state == 1u) {
        a->probe_state = 2u;
        say(a, "DS18B20: 값 끊김 — 가스 온도를 BMP390(함체 안 온도)으로, 보고 26번 0x8000 · J_TMP 선 확인");
    } else if (a->probe_state == 0u && a->up_s >= APP_PROBE_GRACE_S) {
        a->probe_state = 2u;
    }
}

uint32_t app_step(app_t *a)
{
    const uint32_t now = a->hw.now_s(a->hw.ctx);
    if (a->batt_off) {
        return batt_off_step(a, now);
    }
    if (!a->started) {
        a->started = true;
        a->last_s = now;
        a->last_minute = now / 60u;     /* 첫 분은 반쪽이라 다음 분 경계부터 보고 */
    }
    uint32_t dt = now - a->last_s;
    a->last_s = now;
    if ((int32_t)dt < 0) {
        dt = 0;                             /* 시계가 뒤로 감 */
    } else if (dt > APP_MAX_DT_S) {
        dt = APP_MAX_DT_S;
    }
    a->up_s += dt;

    meter_sample_t s;
    const bool got = a->hw.read(a->hw.ctx, &s);
    if (got) {
        if (dt > 1u && a->prev_ok) {
            /* 쉴 때는 10 s 마다 깸 → 이번 값만으로 10 s 를 다 채우면 조리 시작 때 평균 5 s 치를 더 셈.
             * 앞 절반은 지난 값, 뒤 절반은 이번 값으로 (사다리꼴) — 시작 시점이 간격 안 어디든 평균 오차 0 */
            meter_step(&a->meter, &a->prev, 0.5 * (double)dt);
            meter_step(&a->meter, &s, 0.5 * (double)dt);
        } else if (dt > 0u) {
            meter_step(&a->meter, &s, (double)dt);
        }
        a->prev = s;
        a->dp_hist[a->dp_head] = s.dp_raw_pa;
        a->dp_head = (uint16_t)((a->dp_head + 1u) % APP_ZERO_SAMPLES);
        if (a->dp_n < APP_ZERO_SAMPLES) {
            a->dp_n++;
        }
    }

    if (got && s.dp2_ok && a->sdp_n < UINT16_MAX) {
        a->sdp_sum += s.dp2_pa;
        a->sdp_n++;
    }
    if (got && s.t_probe_ok && a->probe_n < UINT16_MAX) {
        a->probe_sum += s.t_probe_c;
        a->probe_n++;
    }
    if (got) {
        probe_watch(a, s.t_probe_ok);
    }
    a->prev_ok = got;
    a->read_ok = got;
    a->baro_held = got && s.patm_held;
    if (a->baro_held) {
        a->baro_held_min = true;
    }

    if (lp_minute_due(a->last_minute, now)) {
        a->last_minute = now / 60u;
        do_minute(a, now);
        if (a->batt_off_pending) {
            batt_off_enter(a, now);
        }
    }
    lora_work(a, now);                  /* 쉬러 가더라도 마지막 보고 송신은 여기서 시작 */
    lte_work(a, true);
    if (!a->batt_off) {
        led_tick(a, now);
    }
    if (a->lte != NULL && (lte_busy(a->lte) || a->lte_want)) {
        return 1000u;                       /* LTE 단계는 1 s 마다 */
    }
    return a->batt_off ? APP_OFF_TICK_MS : lp_tick_ms(meter_is_flowing(&a->meter));
}

bool app_zero(app_t *a)
{
    if (a->batt_off || a->dp_n < 4u) {
        return false;                   /* 쉬는 중(센서 꺼짐) · 너무 적게 읽음 */
    }
    double sum = 0.0;
    for (uint16_t i = 0; i < a->dp_n; i++) {
        sum += a->dp_hist[i];
    }
    const double z = sum / a->dp_n;
    meter_set_zero(&a->meter, z);
    a->settings.zero_pa = z;
    a->settings.zero_set = true;
    flash_on(a, true);
    const bool ok = settings_save(a->nv_set, &a->settings) == NVLOG_OK;
    flash_on(a, false);
    char m[48];
    snprintf(m, sizeof(m), "영점 %.3f Pa %s", z, ok ? "저장" : "저장 실패");
    say(a, m);
    return ok;
}

bool app_batt_cal(app_t *a, uint16_t actual_mv)
{
    const uint16_t mv = a->hw.batt_mv != NULL ? a->hw.batt_mv(a->hw.ctx) : 0u;   /* 지금 보정값이 들어간 값 */
    if (mv == 0u || actual_mv < 2000u || actual_mv > 4500u) {
        say(a, "셀 보정: 못 잼 · 실제 값이 범위 밖 — 안 함");
        return false;
    }
    const int32_t off = (int32_t)a->settings.batt_cal_mv + ((int32_t)actual_mv - (int32_t)mv);
    if (off > APP_BATT_CAL_MAX_MV || off < -APP_BATT_CAL_MAX_MV) {
        say(a, "셀 보정: 차이가 너무 큼 (±300 mV) — 멀티미터 값을 다시 볼 것");
        return false;
    }
    a->settings.batt_cal_mv = (int16_t)off;
    a->settings.batt_cal_set = true;
    flash_on(a, true);
    const bool ok = settings_save(a->nv_set, &a->settings) == NVLOG_OK;
    flash_on(a, false);
    char m[96];
    snprintf(m, sizeof(m), "셀 보정: 잰 값 %u mV → 실제 %u mV, 보정값 %+ld mV %s", (unsigned)mv, (unsigned)actual_mv,
             (long)off, ok ? "저장" : "저장 실패");
    say(a, m);
    return ok;
}

bool app_batt_cal_once(app_t *a, uint16_t actual_mv)
{
    if (a->settings.batt_cal_set) {
        char m[160];
        snprintf(m, sizeof(m), "셀 보정: 이미 있음(%+d mV) — 다시 안 맞춤. 이 판은 BATT_CAL_ACTUAL_MV=0 으로 다시 구울 것",
                 (int)a->settings.batt_cal_mv);
        say(a, m);
        return false;
    }
    return app_batt_cal(a, actual_mv);
}

bool app_set_keys(app_t *a, const uint8_t dev_eui[8], const uint8_t app_eui[8], const uint8_t app_key[16])
{
    if (a->settings.keys_set && memcmp(a->settings.dev_eui, dev_eui, 8) == 0 &&
        memcmp(a->settings.app_eui, app_eui, 8) == 0 && memcmp(a->settings.app_key, app_key, 16) == 0) {
        return true;                    /* 같은 키 — 다시 안 씀 */
    }
    memcpy(a->settings.dev_eui, dev_eui, 8);
    memcpy(a->settings.app_eui, app_eui, 8);
    memcpy(a->settings.app_key, app_key, 16);
    a->settings.keys_set = true;
    flash_on(a, true);
    const bool ok = settings_save(a->nv_set, &a->settings) == NVLOG_OK;
    flash_on(a, false);
    return ok;
}
