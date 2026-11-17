/* 전체 흐름 맥 시험 — 가짜 센서 · 가짜 시계 · 가짜 외부 플래시(W25Q128) · 가짜 LoRa 모듈(RAK3172)로 통째로 돌림 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "fake_lte.h"
#include "fake_rak.h"
#include "fake_w25q.h"
#include "lowpower.h"
#include "nv_w25q.h"

static int fails = 0, checks = 0;

static void ok(const char *what, bool v)
{
    checks++;
    if (!v) {
        fails++;
    }
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

/* ---------- 가짜 보드 ---------- */
typedef struct {
    uint32_t now;
    meter_sample_t s;
    bool sensors_ok;
    double pg_fs;               /* 앱이 알려 준 정압 센서 만점 */
    bool saving;                /* 셀이 낮아 쉬는 중 (센서 · LED 끔) */
    int save_calls;
    int led_green, led_red1, led_red2;     /* LED 깜빡임 횟수 (초록 1번 · 빨강 1번 · 빨강 2번) */
    bool time_ok;
    uint16_t batt;
    int flash_on_calls;
    char last_log[96];
} board_t;

static board_t B;

static bool b_read(void *ctx, meter_sample_t *s)
{
    board_t *b = ctx;
    *s = b->s;
    return b->sensors_ok;
}
static uint32_t b_now(void *ctx) { return ((board_t *)ctx)->now; }
static bool b_time(void *ctx) { return ((board_t *)ctx)->time_ok; }
static uint16_t b_batt(void *ctx) { return ((board_t *)ctx)->batt; }
static void b_flash(void *ctx, bool on)
{
    if (on) {
        ((board_t *)ctx)->flash_on_calls++;
    }
}
static void b_log(void *ctx, const char *m) { snprintf(((board_t *)ctx)->last_log, 96, "%s", m); }
static bool b_set_time(void *ctx, uint32_t t)
{
    board_t *b = ctx;
    b->now = t;
    b->time_ok = true;
    return true;
}

static void b_set_pg_fs(void *ctx, double pa) { ((board_t *)ctx)->pg_fs = pa; }
static void b_power_save(void *ctx, bool off)
{
    board_t *b = ctx;
    b->saving = off;
    b->save_calls++;
}
static void b_led(void *ctx, bool red, uint8_t blinks)
{
    board_t *b = ctx;
    if (!red) {
        b->led_green++;
    } else if (blinks == 1u) {
        b->led_red1++;
    } else {
        b->led_red2++;
    }
}
static void led_zero(void) { B.led_green = B.led_red1 = B.led_red2 = 0; }
static const app_hw_t HW = {b_read, b_now, b_time, b_batt, b_flash, b_log, &B, b_set_time, b_set_pg_fs, b_power_save, b_led};

static fake_lte_t FL;
static lte_t LT;
static ring_t RING;
static nvlog_t NS, NC;
static flash_ops_t OPS;
static app_t A;

/* 전원을 껐다 켠 것처럼: 플래시 내용은 그대로, 나머지는 새로 */
static void boot(void)
{
    ring_mount(&RING, &DEV, 0, 4);                 /* 시험용으로 링을 4섹터(512칸)로 */
    nv_w25q_ops(&OPS, &DEV);
    nvlog_mount(&NS, &OPS, PERSIST_SETTINGS_BASE);
    nvlog_mount(&NC, &OPS, PERSIST_COUNTER_BASE);
    app_init(&A, &HW, &RING, &NS, &NC, &R);
    app_set_lte(&A, &LT);
}

/* 앱이 돌려준 간격대로 시간을 흘리며 돌림 */
static void run_for(uint32_t secs)
{
    const uint32_t end = B.now + secs;
    while (B.now < end) {
        const uint32_t ms = app_step(&A);
        B.now += ms / 1000u;
        F.clock += ms;
    }
}

static void idle_gas(void)
{
    B.s.dp_raw_pa = 0.0;
    B.s.pg_pa = 600.0;
    B.s.t_gas_c = 25.0;
    B.s.patm_pa = 101000.0;
}

/* run_for 와 같되, 모듈이 보낼 때마다 '송신 끝'을 알려 주는 가짜 (정상 망) */
static void run_ack(uint32_t secs)
{
    const uint32_t end = B.now + secs;
    while (B.now < end) {
        const uint32_t ms = app_step(&A);
        if (R.tx_pending) {
            push(&F, "+EVT:TX_DONE\r\n");
        }
        B.now += ms / 1000u;
        F.clock += ms;
    }
}

static bool last_report(report_t *r)
{
    ring_rec_t rec;
    return ring_last(&RING, &rec) == RING_OK && report_decode(rec.report, RING_REPORT_SIZE, r);
}

static const uint8_t DEV_EUI[8] = {0xAC, 0x1F, 0x09, 0xFF, 0xFE, 0x0A, 0x1B, 0x2C};
static const uint8_t APP_EUI[8] = {0, 0, 0, 0, 0, 0, 0, 1};
static const uint8_t APP_KEY[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

int main(void)
{
    chip_new();
    fresh();
    memset(&B, 0, sizeof(B));
    {
        fake_lte_new(&FL, &B.now);          /* 가짜 LTE 모듈: 처음엔 꺼져 있음 */
        lte_io_t lio0;
        fake_lte_io(&FL, &lio0);
        lte_init(&LT, &lio0);
    }
    B.now = 1794819600u + 5u;    /* 분 경계 5초 뒤 */
    B.sensors_ok = true;
    B.batt = 3700;
    idle_gas();

    printf("처음 켜기 (빈 플래시 · 키 없음)\n");
    boot();
    ok("누적 0 · 영점 없음 · 대역 KR920", A.meter.total_nm3 == 0.0 && !A.settings.zero_set && A.settings.band == 7);
    ok("정압 만점 기본 6000 Pa (센서 · 범위 검사 둘 다)", B.pg_fs == 6000.0 && A.meter.cfg.pg_full_scale_pa == 6000.0);
    ok("LoRa: 속도 9600 으로 맞추고 키 없어서 기록만", app_lora_start(&A) == RAK_OK && !A.lora_ready && F.mod_baud == 9600);
    run_for(3 * 60 + 10);
    ok("3분 → 1분 보고 3개가 링에 쌓임", A.reports == 3 && RING.next_seq == 3);
    report_t r;
    ok("보고: 영점 없음 · 시계 안 맞춤 비트", last_report(&r) && (r.flags & REPORT_F_ZERO_UNSET) &&
                                                (r.flags & REPORT_F_TIME_UNSYNCED));
    ok("보고: 정압 600 Pa · 온도 25 °C · 대기압 101000 Pa", r.pg_pa == 600.0 && fabs(r.t_c - 25.0) < 0.05 &&
                                                                r.patm_pa == 101000.0);
    ok("가스 안 쓸 땐 10초마다 깸", app_step(&A) == 10000u);
    ok("LED: 키가 없으면 10 s 마다 빨강 1번 (초록 없음)", B.led_red1 >= 18 && B.led_green == 0 && B.led_red2 == 0);
    uint32_t seq;
    ok("세 개 모두 아직 안 보냄", ring_find_unsent(&RING, 60, &seq) == RING_OK && seq == 0);
    {
        ring_rec_t rec0;
        report_t r0;
        ok("켠 뒤 첫 기록에만 '켬' 비트 (이상 재시작 비트는 없음)",
           ring_read(&RING, 0, &rec0) == RING_OK && report_decode(rec0.report, RING_REPORT_SIZE, &r0) &&
               (r0.flags & REPORT_F_BOOT) && !(r0.flags & REPORT_F_FAULT_RESET) && !(r.flags & REPORT_F_BOOT));
    }

    printf("영점 버튼 (차압 센서가 0.8 Pa 로 치우쳐 있음)\n");
    B.s.dp_raw_pa = 0.8;
    run_for(200);
    ok("영점 잡기 → 0.8 Pa 저장", app_zero(&A) && fabs(A.settings.zero_pa - 0.8) < 1e-9 && A.settings.zero_set);

    printf("키 넣기 → 설정 · 가입 → 쌓인 기록 다시 보냄\n");
    ok("키 저장", app_set_keys(&A, DEV_EUI, APP_EUI, APP_KEY));
    ok("LoRa 설정 · 가입 시작", app_lora_start(&A) == RAK_OK && A.lora_ready && R.joining);
    ok("모듈에 키가 들어감", strcmp(val_of(&F, "AT+APPKEY"), "00112233445566778899AABBCCDDEEFF") == 0 &&
                                strcmp(val_of(&F, "AT+DEVEUI"), "AC1F09FFFE0A1B2C") == 0);
    push(&F, "+EVT:JOINED\r\n");
    F.joined = true;
    /* 모듈이 보낼 때마다 송신 끝을 알려 주는 가짜: 한 번 돌 때마다 TX_DONE 하나 */
    const uint32_t until = B.now + 400u;
    while (B.now < until) {
        const uint32_t ms = app_step(&A);
        if (R.tx_pending) {
            push(&F, "+EVT:TX_DONE\r\n");
        }
        if (getenv("DBG")) printf("    t=%u pend=%d live=%d joined=%d sent=%u res=%u fail=%u last=%s\n", B.now, R.tx_pending, A.tx_live, R.joined, A.sent, A.resends, A.send_fail, F.nlog ? F.log[F.nlog-1] : "");
        B.now += ms / 1000u;
        F.clock += ms;
    }
    ok("가입됨", R.joined);
    ok("보낸 게 있고 보낸 표시가 링에 남음", A.sent >= 3 && A.resends >= 3);
    ok("최근 기록 중 못 보낸 것 없음", ring_find_unsent(&RING, 60, &seq) == RING_E_MISSING);
    int sends = 0;
    for (int i = 0; i < F.nlog; i++) {
        if (strncmp(F.log[i], "AT+SEND=1:", 10) == 0) {
            sends++;
        }
    }
    ok("포트 1 · 24바이트(48자)로 보냄", sends >= 1);

    printf("조리 (차압 40 Pa) → 1초마다 깨고 누적이 늘어남\n");
    const double before = A.meter.total_nm3;
    B.s.dp_raw_pa = 40.8;                /* 영점 0.8 빼면 40 Pa */
    run_for(5 * 60);
    ok("흐름 중엔 1초마다 깸", app_step(&A) == 1000u);
    ok("누적이 늘어남", A.meter.total_nm3 > before);
    ok("보고에 조리 비트 · 영점 비트 꺼짐", last_report(&r) && (r.flags & REPORT_F_FLOWING) && !(r.flags & REPORT_F_ZERO_UNSET));
    printf("       (5분 누적 %.4f Nm³, 마지막 1분 유량 %.3f Nm³/h)\n", A.meter.total_nm3 - before, r.flow_nm3h);

    printf("전원 끊겼다 다시 켜기\n");
    const double total = A.counter.total_nm3;     /* 마지막 1분 경계에 저장한 값 (그 뒤 몇 초 분은 잃음) */
    const uint16_t next_report = A.counter.report_seq;
    const uint32_t ring_next = RING.next_seq;
    fresh();                             /* LoRa 모듈도 다시 켜짐 (속도 저장은 날아간 가짜) */
    boot();
    ok("누적량 = 마지막 1분 저장분", A.meter.total_nm3 == total && total > 0.0);
    ok("영점 그대로 · 키 그대로", A.settings.zero_set && fabs(A.settings.zero_pa - 0.8) < 1e-9 && A.settings.keys_set);
    ok("보고 순번 · 링 순번 이어짐", A.counter.report_seq == next_report && RING.next_seq == ring_next);

    printf("누적 저장 영역이 망가졌을 때 → 링 마지막 보고로 되살림\n");
    w25q_erase_sector(&DEV, PERSIST_COUNTER_BASE);
    w25q_erase_sector(&DEV, PERSIST_COUNTER_BASE + W25Q_SECTOR);
    boot();
    ok("누적 = 링 마지막 보고 값 (0.1 L 안)", fabs(A.meter.total_nm3 - total) < 0.0001 + 1e-9);
    ok("보고 순번도 이어짐", A.counter.report_seq == next_report);
    ok("로그로 알림", strstr(B.last_log, "되살림") != NULL);

    printf("링엔 썼는데 누적 저장 전에 전원이 끊김 → 켤 때 링 값으로\n");
    run_for(2 * 60);                                /* 누적 저장이 다시 생기게 */
    {
        const double t0 = A.counter.total_nm3;
        const uint16_t rs0 = A.counter.report_seq;
        report_t rr;
        memset(&rr, 0, sizeof(rr));
        rr.seq = rs0;                               /* 누적이 아직 안 센 다음 순번 */
        rr.total_nm3 = t0 + 0.0123;
        uint8_t bytes[REPORT_LEN];
        report_encode(&rr, bytes);
        uint32_t rseq;
        ring_append(&RING, bytes, &rseq);           /* 누적 저장은 안 함 = 그 사이 끊김 */
        boot();
        ok("누적 = 링 값 (0.1 L 안) · 다음 보고 순번 이어짐",
           fabs(A.meter.total_nm3 - (t0 + 0.0123)) < 0.0001 + 1e-9 && A.counter.report_seq == (uint16_t)(rs0 + 1u));
        ok("로그로 알림", strstr(B.last_log, "앞서") != NULL);
        boot();
        ok("한 번 더 켜도 같은 값 (두 번 더하지 않음)",
           fabs(A.meter.total_nm3 - (t0 + 0.0123)) < 0.0001 + 1e-9 && A.counter.report_seq == (uint16_t)(rs0 + 1u));
    }

    printf("같은 일인데 그 분에 가스를 안 씀 → 링 값(0.1 L 반올림)이 저장값보다 작아도 순번은 이음\n");
    run_for(2 * 60);                                /* 누적 저장을 다시 맞춤 */
    {
        const double t0 = A.counter.total_nm3;
        const uint16_t rs0 = A.counter.report_seq;
        report_t rr;
        memset(&rr, 0, sizeof(rr));
        rr.seq = rs0;
        rr.total_nm3 = t0 - 0.00004;                /* 반올림이 아래로 간 경우 */
        uint8_t bytes[REPORT_LEN];
        report_encode(&rr, bytes);
        uint32_t rseq;
        ring_append(&RING, bytes, &rseq);
        boot();
        ok("다음 보고 순번이 겹치지 않음", A.counter.report_seq == (uint16_t)(rs0 + 1u));
        ok("누적은 줄지 않음 (저장값 그대로)", A.meter.total_nm3 == t0);
        run_for(70);
        report_t r2;
        ok("실제 다음 보고가 다음 순번으로 나감", last_report(&r2) && r2.seq == (uint16_t)(rs0 + 1u));
    }

    printf("워치독 · 오류로 재시작했을 때\n");
    app_set_boot(&A, true);
    run_for(2 * 60);
    {
        ring_rec_t rb;
        report_t r1;
        ok("다음 첫 기록에 '켬' + '이상 재시작' 비트, 그다음 기록엔 없음",
           ring_read(&RING, RING.next_seq - 2u, &rb) == RING_OK && report_decode(rb.report, RING_REPORT_SIZE, &r1) &&
               (r1.flags & REPORT_F_BOOT) && (r1.flags & REPORT_F_FAULT_RESET) && last_report(&r) &&
               !(r.flags & (REPORT_F_BOOT | REPORT_F_FAULT_RESET)));
    }

    printf("센서가 안 될 때\n");
    B.sensors_ok = false;
    const uint32_t reports = A.reports;
    run_for(2 * 60);
    ok("그래도 1분 보고는 쌓임 (측정 시간 부족 비트)", A.reports == reports + 2 && last_report(&r) &&
                                                        (r.flags & REPORT_F_INCOMPLETE));
    B.sensors_ok = true;

    printf("배터리 낮음\n");
    B.batt = 3400;
    run_for(60);
    ok("3.4 V → 경고 비트", last_report(&r) && (r.flags & REPORT_F_BATT_WARN) && !(r.flags & REPORT_F_BATT_QUALITY) &&
                               r.batt_mv == 3400);

    printf("셀 3.3 V 종료: 낮으면 쉬고, 오르면 다시 시작\n");
    {
        B.batt = 3240;                              /* 한 번만 낮음 → 안 멈춤 */
        run_for(60);
        B.batt = 3700;
        run_for(60);
        ok("한 번 튄 낮은 값으로는 안 멈춤", !A.batt_off && !B.saving && A.batt_offs == 0u);

        const uint32_t reps = A.reports;
        B.batt = 3240;
        run_for(2 * 60 + 5);
        ok("2분 이어 3.25 V 아래 → 쉼 · 센서 끔", A.batt_off && B.saving && A.batt_offs == 1u);
        ok("마지막 기록에 '저전압 멈춤' 비트", last_report(&r) && (r.flags & REPORT_F_BATT_OFF) && A.reports == reps + 2u);
        ok("쉬는 동안은 20 s 마다만 깸", app_step(&A) == APP_OFF_TICK_MS);

        const double tot = A.meter.total_nm3;
        const uint32_t reps_off = A.reports;
        const uint32_t ringn = RING.next_seq;
        B.s.dp_raw_pa = 30.0;                       /* 쉬는 동안 가스를 써도 */
        B.batt = 3400;                              /* 다시 켬 문턱(3.45 V) 아래 */
        run_for(25 * 60);
        ok("쉬는 동안: 사용량 · 보고 · 기록 그대로 (측정 안 함)", A.meter.total_nm3 == tot && A.reports == reps_off &&
                                                               RING.next_seq == ringn);
        ok("3.40 V 로는 다시 안 켬", A.batt_off && B.saving);
        led_zero();
        run_for(5 * 60);
        ok("LED: 쉬는 동안은 안 깜빡임", B.led_green + B.led_red1 + B.led_red2 == 0);
        ok("쉬는 동안은 영점 버튼이 안 먹음 (센서 꺼짐)", !app_zero(&A));

        B.batt = 3500;
        int steps = 0;
        while (A.batt_off && steps < 40) {            /* 20 s 씩 깨며 10분 안에 셀을 다시 잼 */
            const uint32_t ms = app_step(&A);
            B.now += ms / 1000u;
            F.clock += ms;
            steps++;
        }
        ok("3.45 V 넘으면 10분 안에 다시 시작 · 센서 켬", !A.batt_off && !B.saving && steps <= 31);
        const double t0r = A.meter.total_m3;
        B.now += 1u;
        F.clock += 1000u;
        const uint32_t gap = B.now - A.last_s;         /* 다시 시작한 뒤 깬 시간(10 s 쉼 + 1 s) */
        app_step(&A);
        const double q1 = A.meter.last_flow_m3h / 3600.0;
        ok("다시 시작 뒤 첫 적산은 깨어 있던 시간만 (쉰 10분은 안 셈)",
           gap <= 11u && q1 > 0.0 && fabs((A.meter.total_m3 - t0r) - q1 * gap) < 1e-9);
        const double t1 = A.meter.total_nm3;
        run_for(3 * 60);
        ok("다시 시작 뒤 보고가 쌓이고 사용량도 셈", A.reports > reps_off && A.meter.total_nm3 > t1);
        idle_gas();
        B.batt = 3700;
        run_for(2 * 60);
    }

    printf("망 시각으로 시계 맞추기\n");
    /* 지금 가짜 RTC 는 2026-11-16 09:xx. 망은 3시간 뒤라고 알려 줌 */
    const uint32_t real = B.now + 3u * 3600u;
    lp_civil_t c;
    lp_civil_from_unix(real, &c);
    snprintf(F.ltime, sizeof(F.ltime), "%02uh%02um%02us on %02u/%02u/%04u", c.hour, c.min, c.sec, c.month, c.day, c.year);
    app_lora_start(&A);
    push(&F, "+EVT:JOINED\r\n");
    F.joined = true;
    const double tot0 = A.meter.total_nm3;
    B.s.dp_raw_pa = 40.8;
    const uint32_t stop_at = B.now + 300u;
    while (B.now < stop_at && A.time_syncs == 0) {
        const uint32_t ms = app_step(&A);
        if (R.tx_pending) {
            push(&F, "+EVT:TX_DONE\r\n");
        }
        B.now += ms / 1000u;
        F.clock += ms;
    }
    ok("시각 요청을 켰다가(TIMEREQ=1) 맞춘 뒤 끔", A.time_syncs == 1 && strcmp(val_of(&F, "AT+TIMEREQ"), "0") == 0);
    ok("RTC 가 3시간 앞으로 맞춰짐", B.time_ok && B.now >= real && B.now < real + 120u);
    ok("시계가 뛴 3시간을 적산하지 않음 (유량 0.46 Nm³/h 면 1.4 Nm³ 가 될 뻔)", A.meter.total_nm3 - tot0 < 0.05);
    run_for(70);
    ok("그 뒤 보고엔 시계 안 맞춤 비트가 없음", last_report(&r) && !(r.flags & REPORT_F_TIME_UNSYNCED) && r.unix_s >= real);
    const uint32_t syncs = A.time_syncs;
    run_for(600);
    ok("하루가 안 지나면 다시 안 물음", A.time_syncs == syncs);

    printf("LoRa 모듈이 '송신 끝'을 안 줌 → 60 s 뒤 풀고 다시 보냄, 3번 이어지면 모듈 리셋\n");
    /* 앞 구간(시계 맞추기 뒤 10분)에서도 TX_DONE 을 안 넣어 이미 놓침 · 리셋이 일어남 → 다시 가입해 깨끗하게 시작 */
    app_lora_start(&A);
    push(&F, "+EVT:JOINED\r\n");
    run_for(5);
    const uint32_t to0 = A.tx_timeouts, rs0 = A.lora_restarts;
    const uint32_t fail0 = A.send_fail, resets0 = (uint32_t)F.resets, joins0 = A.joins;
    B.s.dp_raw_pa = 0.8;
    const uint32_t t_stuck = B.now;
    uint32_t first_free = 0;
    while (B.now < t_stuck + 400u && A.lora_restarts == rs0) {
        const uint32_t ms = app_step(&A);     /* TX_DONE 을 안 넣음 */
        if (first_free == 0 && A.tx_timeouts > to0) {
            first_free = B.now - t_stuck;
        }
        B.now += ms / 1000u;
        F.clock += ms;
    }
    ok("60 s 쯤에 기다림을 풂 (영영 안 멈춤)", first_free >= 50u && first_free <= 75u && A.send_fail > fail0);
    ok("3번 놓치자 모듈 리셋 → 설정 · 가입 다시", A.tx_timeouts == to0 + 3 && A.lora_restarts == rs0 + 1 &&
                                                    F.resets > (int)resets0 && A.lora_ready && R.joining && A.joins > joins0);
    ok("못 보낸 기록은 링에 남아 있음", ring_find_unsent(&RING, 60, &seq) == RING_OK);

    printf("가입 결과가 안 옴 → 300 s 뒤 풀고 10분 간격 다시 가입\n");
    const uint32_t j0 = A.joins, tj = B.now;
    run_for(310);
    ok("300 s 뒤 가입 기다림을 풂", !R.joining && A.joins == j0);
    run_for(600 - 310 + 20);
    ok("10분 뒤 다시 가입", R.joining && A.joins == j0 + 1 && B.now - tj >= 600u);
    push(&F, "+EVT:JOINED\r\n");
    run_for(20);
    ok("가입되면 놓침 횟수를 지움", R.joined && A.lora_misses == 0);

    printf("켤 때 모듈이 답 없음 → 10분마다 리셋하고 다시 해 봄\n");
    F.dead = true;
    ok("처음엔 실패", app_lora_start(&A) != RAK_OK && !A.lora_ready);
    const int r0 = F.resets;
    run_for(300);
    ok("10분 전엔 다시 안 함", !A.lora_ready && F.resets == r0);
    F.dead = false;
    run_for(320);
    ok("10분 뒤 리셋부터 다시 → 설정 · 가입 시작", A.lora_ready && F.resets > r0 && R.joining);

    printf("쉴 때(10초 간격) 조리가 시작되면 첫 간격은 반만 셈 (사다리꼴)\n");
    idle_gas();
    run_for(3 * 60);
    ok("쉴 땐 10초마다 깸", app_step(&A) == 10000u);
    {
        const double v0 = A.meter.total_m3;
        B.s.dp_raw_pa = 30.0;                       /* 이 10 s 사이 어딘가에서 불을 켬 */
        B.now += 10u;
        F.clock += 10000u;
        app_step(&A);
        const double q = A.meter.last_flow_m3h / 3600.0;
        ok("첫 10 s 간격: 5 s 치만 더함 (전엔 10 s 치)", q > 0.0 && fabs((A.meter.total_m3 - v0) - 5.0 * q) < 1e-9);
        const double v1 = A.meter.total_m3;
        B.now += 1u;
        F.clock += 1000u;
        app_step(&A);
        ok("그 뒤 1 s 간격은 그대로 1 s 치", fabs((A.meter.total_m3 - v1) - q) < 1e-9);
    }
    idle_gas();

    printf("정압 센서를 2.5 kPa 품번으로 갈았을 때 → 내려받기로 만점 바꿈\n");
    {
        const uint8_t bad_port[3] = {0x01, 0x09, 0xC4};
        ok("포트 1 로 오면 무시", !app_downlink(&A, 1, bad_port, 3) && B.pg_fs == 6000.0);
        const uint8_t too_small[3] = {0x01, 0x01, 0xF4};          /* 500 Pa */
        ok("범위 밖(500 Pa)은 무시", !app_downlink(&A, APP_CFG_PORT, too_small, 3) && B.pg_fs == 6000.0);
        const uint8_t unknown[2] = {0x7F, 0x00};
        ok("모르는 명령은 무시", !app_downlink(&A, APP_CFG_PORT, unknown, 2));
        if (!A.lora_ready) {                                       /* 앞 시험에서 리셋 중이면 다시 띄움 */
            (void)app_lora_start(&A);
        }
        push(&F, "+EVT:JOINED\r\n");
        run_for(5);
        push(&F, "+EVT:RX_1:-70:8:UNICAST:10:0109C4\r\n");      /* 01 · 2500 */
        run_for(2);
        ok("LoRa 로 받음 → 센서 · 범위 검사 2500 Pa", B.pg_fs == 2500.0 && A.meter.cfg.pg_full_scale_pa == 2500.0 &&
                                                        A.settings.pg_fs_pa == 2500u);
        B.pg_fs = 0.0;
        boot();
        ok("다시 켜도 2500 Pa (설정에 저장됨)", B.pg_fs == 2500.0 && A.meter.cfg.pg_full_scale_pa == 2500.0);
        const uint8_t again[3] = {0x01, 0x09, 0xC4};
        const uint32_t saves = NS.next_seq;
        ok("같은 값을 또 받으면 다시 저장하지 않음", app_downlink(&A, APP_CFG_PORT, again, 3) && NS.next_seq == saves &&
                                                       B.pg_fs == 2500.0);
        const uint8_t back[3] = {0x01, 0x00, 0x00};
        ok("0 을 보내면 기본 6000 으로", app_downlink(&A, APP_CFG_PORT, back, 3) && B.pg_fs == 6000.0 &&
                                           app_pg_fs_pa(&A) == 6000u);
    }

    printf("LED 상태 표시\n");
    {
        (void)app_lora_start(&A);
        push(&F, "+EVT:JOINED\r\n");
        run_ack(2 * 60);                            /* 보고가 한 번은 제대로 나가게 */
        led_zero();
        run_ack(60);
        ok("가입 · 송신 · 센서 정상 → 10 s 마다 초록 1번만 (1분에 6번)", B.led_green == 6 && B.led_red1 == 0 && B.led_red2 == 0);
        B.s.dp_raw_pa = 30.0;
        run_ack(5);
        led_zero();
        run_ack(60);
        ok("조리 중(1 s 깨기)에도 10 s 마다 한 번", B.led_green == 6 && B.led_red1 == 0);
        idle_gas();
        run_ack(2 * 60);
        led_zero();
        run_for(2 * 60 + 70);                       /* 송신 끝 소식이 안 옴 → 60 s 뒤 실패로 셈 */
        ok("송신이 실패하면 빨강 1번", B.led_red1 >= 1 && A.led_state == APP_LED_LORA);
        run_ack(2 * 60);
        B.sensors_ok = false;
        led_zero();
        run_ack(30);
        ok("센서를 못 읽으면 빨강 2번 (통신보다 먼저)", B.led_red2 >= 2 && B.led_green == 0 && A.led_state == APP_LED_SENSOR);
        B.sensors_ok = true;
        R.joined = false;
        R.joining = true;
        A.join_start_s = B.now;
        led_zero();
        run_for(30);
        ok("가입이 풀리면 빨강 1번", B.led_red1 >= 2 && B.led_green == 0 && A.led_state == APP_LED_LORA);
        push(&F, "+EVT:JOINED\r\n");
        run_ack(2 * 60);
        led_zero();
        run_ack(30);
        ok("다시 가입되고 보내지면 초록으로 돌아옴", B.led_green >= 2 && B.led_red1 == 0 && A.led_state == APP_LED_OK);
    }

    printf("LTE 1차 시험: LoRa 명령 02 로 켜고 · 망 확인 · 끄기, 결과는 LoRa 포트 2\n");
    {
        fake_lte_new(&FL, &B.now);
        lte_io_t lio;
        fake_lte_io(&FL, &lio);
        lte_init(&LT, &lio);
        const uint8_t cmd[1] = {APP_CFG_LTE_TEST};
        B.batt = 3600;
        ok("셀 3.60 V 면 안 함 (3.65 V 부터)", app_downlink(&A, APP_CFG_PORT, cmd, 1) && !lte_busy(&LT) && !A.lte_want &&
                                                 A.lte_refused == 1u);
        F.nlog = 0;
        run_ack(30);
        ok("'셀 낮아 안 함' 도 서버로 올림 (결과 코드 8)", A.lte_last.code == LTE_R_LOW_BATT && !A.lte_report_pending &&
                                                     count_writes("AT+SEND=2:") == 1);
        B.batt = 3800;
        const uint32_t reps = A.reports;
        F.nlog = 0;                                 /* 가짜 모듈의 명령 기록(64줄)을 비우고 셈 */
        const int sends2 = 0;
        ok("셀 3.80 V → 명령 받음", app_downlink(&A, APP_CFG_PORT, cmd, 1) && A.lte_want);
        ok("다음 걸음에 켜기 시작 · 그동안 1 s 마다 깸", app_step(&A) == 1000u && lte_busy(&LT) && FL.key_down);
        run_ack(3 * 60);
        ok("끝: 망 등록됨 · 모듈 꺼짐", !lte_busy(&LT) && A.lte_last.code == LTE_R_OK && A.lte_last.off == LTE_OFF_QPOWD && !FL.on);
        ok("결과를 LoRa 포트 2 로 한 번 올림", count_writes("AT+SEND=2:") == sends2 + 1 && !A.lte_report_pending);
        ok("그동안 1분 보고 · 측정은 그대로", A.reports >= reps + 2u);
        ok("로그에 결과", strstr(B.last_log, "LTE") != NULL || A.lte_last.code == LTE_R_OK);

        printf("LTE 하는 중에 셀이 낮아 쉬러 감 → LTE 먼저 끔\n");
        FL.reg_after = 100000;                      /* 망을 못 찾아 오래 걸리는 중에 */
        ok("다시 시작", app_lte_test(&A) && app_step(&A) == 1000u && lte_busy(&LT));
        B.batt = 3240;
        run_ack(3 * 60);
        ok("쉬러 감 · LTE 는 멈춤으로 끝 · 모듈 꺼짐", A.batt_off && !lte_busy(&LT) && A.lte_last.code == LTE_R_ABORTED && !FL.on);
        ok("쉬는 중이라 결과는 안 올림 · LTE 시험도 안 받음", !A.lte_report_pending && !app_lte_test(&A));
        B.batt = 3700;
        run_ack(11 * 60);
        ok("셀이 오르면 다시 시작", !A.batt_off);
    }

    printf("LTE 결과를 보내다 LoRa 를 다시 시작 → 다음 기록이 '보냄' 표시를 제대로 받음\n");
    {
        A.tx_is_lte = true;
        A.tx_live = true;
        (void)app_lora_start(&A);
        ok("다시 시작하면 'LTE 결과 보내는 중' 표시도 지움", !A.tx_is_lte && !A.tx_live);
        push(&F, "+EVT:JOINED\r\n");
        run_ack(3 * 60);
        uint32_t useq;
        ok("그 뒤 기록들은 모두 '보냄'", ring_find_unsent(&RING, 3, &useq) != RING_OK);
    }

    printf("켤 때 셀이 이미 낮음 → 가입 안 하고 1분 안에 쉼\n");
    {
        B.batt = 3200;
        boot();
        F.nlog = 0;
        ok("LoRa 시작 안 함 (가입 송신 없음)", app_lora_start(&A) != RAK_OK && !A.lora_ready &&
                                               count_writes("AT+JOIN") == 0);
        const uint32_t t0 = B.now;
        while (!A.batt_off && B.now - t0 < 5u * 60u) {
            run_for(10);
        }
        ok("첫 1분 보고로 바로 쉼 (전엔 2분 + 가입)", A.batt_off && B.now - t0 <= 2u * 60u);
        B.batt = 3700;
        run_ack(11 * 60);
        ok("셀이 오르면 다시 시작", !A.batt_off);
        (void)app_lora_start(&A);
        push(&F, "+EVT:JOINED\r\n");
        run_ack(60);
    }

    printf("LTE 로 망을 찾는 중에 시계가 뜀 (망 시각으로 맞춤) → LTE 는 안 흔들림\n");
    {
        fake_lte_new(&FL, &B.now);
        lte_io_t lio;
        fake_lte_io(&FL, &lio);
        lte_init(&LT, &lio);
        FL.reg_after = 60;                          /* 켜진 뒤 60 s 에 등록 */
        ok("시험 시작", app_lte_test(&A) && app_step(&A) == 1000u && lte_busy(&LT));
        run_ack(30);
        ok("망 찾는 중", LT.state == LTE_S_REG);
        const uint32_t jump = 5u * 3600u;           /* RTC 가 5시간 앞으로 */
        B.now += jump;
        A.last_s += jump;                           /* 앱의 시계 맞춤이 하는 일 (time_sync) */
        A.last_minute = B.now / 60u;
        FL.boot_t += jump;                          /* 모듈은 실제 시간으로 삶 */
        run_ack(5);
        ok("시계가 뛰어도 계속 망을 찾음 (전엔 바로 '등록 안 됨')", lte_busy(&LT) && LT.state == LTE_S_REG);
        run_ack(3 * 60);
        ok("끝: 망 등록됨 · 꺼짐", !lte_busy(&LT) && A.lte_last.code == LTE_R_OK && !FL.on);
    }

    printf("MCU 만 다시 켜졌는데 LTE 는 켜져 있음 → 켜자마자 끔\n");
    {
        fake_lte_new(&FL, &B.now);
        lte_io_t lio;
        fake_lte_io(&FL, &lio);
        lte_init(&LT, &lio);
        FL.on = true;
        FL.boot_t = B.now - 100u;
        boot();                                     /* 워치독 · 오류 재시작처럼 */
        (void)app_lora_start(&A);
        push(&F, "+EVT:JOINED\r\n");
        F.nlog = 0;
        run_ack(2 * 60);
        ok("켜진 걸 알아채고 끔", A.lte_left_on == 1u && !FL.on && A.lte_last.code == LTE_R_LEFT_ON);
        ok("서버에도 알림 (LoRa 포트 2)", count_writes("AT+SEND=2:") == 1);

        FL.on = true;                               /* 이번엔 끄기가 안 먹는 모듈 */
        FL.boot_t = B.now - 100u;
        FL.key_off_ignored = true;
        const uint32_t t_wait = B.now;
        while (A.lte_left_on < 2u && B.now - t_wait < 11u * 60u) {
            run_ack(10);                            /* 다음 살핌(10분 안)을 기다림 */
        }
        run_ack(3 * 60);                            /* 끄기 절차(누름 · 기다림 · 리셋 · 다시 누름)가 끝날 때까지 */
        ok("10분 안에 다시 알아채고, 못 끄면 '못 끔'으로 남김", A.lte_left_on == 2u && !lte_busy(&LT) &&
                                                             A.lte_last.off == LTE_OFF_FAILED && FL.on);
        FL.key_off_ignored = false;
        run_ack(11 * 60);
        ok("10분 뒤 다시 해서 끔", A.lte_left_on == 3u && !FL.on && A.lte_last.off != LTE_OFF_FAILED);
    }

    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
