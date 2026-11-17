/* 전체 흐름 맥 시험 — 가짜 센서 · 가짜 시계 · 가짜 외부 플래시(W25Q128) · 가짜 LoRa 모듈(RAK3172)로 통째로 돌림 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
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
static const app_hw_t HW = {b_read, b_now, b_time, b_batt, b_flash, b_log, &B, b_set_time, b_set_pg_fs};

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
        const uint8_t back[3] = {0x01, 0x00, 0x00};
        ok("0 을 보내면 기본 6000 으로", app_downlink(&A, APP_CFG_PORT, back, 3) && B.pg_fs == 6000.0 &&
                                           app_pg_fs_pa(&A) == 6000u);
    }

    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
