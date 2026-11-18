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
    if (a->settings.zero_set) {
        meter_set_zero(&a->meter, a->settings.zero_pa);
    }

    if (counter_load(nv_ctr, &a->counter) == NVLOG_OK) {
        a->meter.total_nm3 = a->counter.total_nm3;
        a->meter.total_m3 = a->counter.total_m3;
        a->meter.vent_minutes = a->counter.vent_minutes;
        a->meter.depletions = a->counter.depletions;
        a->meter.events.count = a->counter.events;
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

uint16_t app_pg_fs_pa(const app_t *a)
{
    return a->settings.pg_fs_pa != 0u ? a->settings.pg_fs_pa : (uint16_t)APP_PG_FS_DEFAULT_PA;
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
        a->tx_is_lte = false;
        if (a->rak->last_tx == RAK_EV_TX_DONE || a->rak->last_tx == RAK_EV_CONFIRMED_OK) {
            if (!was_lte) {
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
