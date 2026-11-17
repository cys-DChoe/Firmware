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
    meter_init(&a->meter, &cfg);
    if (a->settings.zero_set) {
        meter_set_zero(&a->meter, a->settings.zero_pa);
    }

    if (counter_load(nv_ctr, &a->counter) == NVLOG_OK) {
        a->meter.total_nm3 = a->counter.total_nm3;
        a->meter.total_m3 = a->counter.total_m3;
        a->meter.vent_minutes = a->counter.vent_minutes;
        a->meter.depletions = a->counter.depletions;
        a->meter.events.count = a->counter.events;
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
    rak_give_up(a->rak);                    /* 처음부터 다시: 전에 기다리던 소식은 버림 */
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
    const uint16_t mv = a->hw.batt_mv != NULL ? a->hw.batt_mv(a->hw.ctx) : 0u;
    if (mv != 0u) {
        a->batt_flags = report_battery_flags(mv, a->batt_flags);
        f |= a->batt_flags;
    }
    r->batt_mv = mv;
    r->flags = f;
}

static void do_minute(app_t *a, uint32_t now)
{
    meter_minute_t mm;
    meter_minute(&a->meter, &mm);
    report_t r;
    build_report(a, &mm, now, &r);
    uint8_t bytes[REPORT_LEN];
    report_encode(&r, bytes);

    flash_on(a, true);
    uint32_t seq;
    if (ring_append(a->ring, bytes, &seq) == RING_OK) {
        a->fresh_pending = true;
        a->fresh_seq = seq;
    } else {
        say(a, "링 저장 실패");
    }
    a->counter.total_nm3 = a->meter.total_nm3;
    a->counter.total_m3 = a->meter.total_m3;
    a->counter.ring_next_seq = a->ring->next_seq;
    a->counter.report_seq = (uint16_t)(r.seq + 1u);
    a->counter.vent_minutes = a->meter.vent_minutes;
    a->counter.depletions = a->meter.depletions;
    a->counter.events = a->meter.events.count;
    a->counter.unix_s = now;
    if (counter_save(a->nv_ctr, &a->counter) != NVLOG_OK) {
        say(a, "누적 저장 실패");
    }
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
        /* 시계가 뛴 만큼 기준도 옮겨서 큰 간격을 적산하지 않게 */
        a->last_s += (uint32_t)diff;
        a->last_minute = t / 60u;
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
        if (a->rak->last_tx == RAK_EV_TX_DONE || a->rak->last_tx == RAK_EV_CONFIRMED_OK) {
            flash_on(a, true);
            ring_mark_sent(a->ring, a->tx_seq);
            flash_on(a, false);
            a->sent++;
            a->lora_misses = 0;
            time_sync(a, now);
        } else {
            a->send_fail++;          /* 링에 "안 보냄"으로 남아 나중에 다시 보냄 */
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

uint32_t app_step(app_t *a)
{
    const uint32_t now = a->hw.now_s(a->hw.ctx);
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

    meter_sample_t s;
    if (a->hw.read(a->hw.ctx, &s)) {
        if (dt > 0u) {
            meter_step(&a->meter, &s, (double)dt);
        }
        a->dp_hist[a->dp_head] = s.dp_raw_pa;
        a->dp_head = (uint16_t)((a->dp_head + 1u) % APP_ZERO_SAMPLES);
        if (a->dp_n < APP_ZERO_SAMPLES) {
            a->dp_n++;
        }
    }

    if (lp_minute_due(a->last_minute, now)) {
        a->last_minute = now / 60u;
        do_minute(a, now);
    }
    lora_work(a, now);
    return lp_tick_ms(meter_is_flowing(&a->meter));
}

bool app_zero(app_t *a)
{
    if (a->dp_n < 4u) {
        return false;                   /* 너무 적게 읽음 */
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
