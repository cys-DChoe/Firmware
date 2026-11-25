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
    bool sdp_on;                /* 앱이 알려 준 SDP810 켜짐 (설정 04) */
    int sdp_calls;
    int save_calls;
    int led_green, led_red1, led_red2, led_red3;   /* LED 깜빡임 횟수 (초록 1번 · 빨강 1 · 2 · 3번) */
    bool time_ok;
    uint16_t batt;
    int flash_on_calls;
    char last_log[96];
    int minute_logs;            /* "1분 #" 로 시작하는 로그 수 */
    char minute_log[352];
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
static app_t A;
/* 실제 보드처럼 잰 값에 저장된 보정값이 더해짐 */
static uint16_t b_batt(void *ctx)
{
    const board_t *b = ctx;
    return b->batt != 0u ? (uint16_t)(b->batt + A.settings.batt_cal_mv) : 0u;
}
static void b_flash(void *ctx, bool on)
{
    if (on) {
        ((board_t *)ctx)->flash_on_calls++;
        (void)w25q_init(&DEV);                     /* 보드 hw_flash 와 같게 실제로 깨우고 재움 (11/24 검수 2차) */
    } else {
        (void)w25q_sleep(&DEV);
    }
}
static void b_log(void *ctx, const char *m)
{
    board_t *b = ctx;
    snprintf(b->last_log, sizeof(b->last_log), "%s", m);
    if (strncmp(m, "1분 #", strlen("1분 #")) == 0) {
        b->minute_logs++;
        snprintf(b->minute_log, sizeof(b->minute_log), "%s", m);
    }
}
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
    } else if (blinks == 2u) {
        b->led_red2++;
    } else {
        b->led_red3++;
    }
}
static void b_set_sdp(void *ctx, bool on)
{
    board_t *b = ctx;
    b->sdp_on = on;
    b->sdp_calls++;
}
static void led_zero(void) { B.led_green = B.led_red1 = B.led_red2 = B.led_red3 = 0; }
static const app_hw_t HW = {b_read, b_now, b_time, b_batt, b_flash, b_log, &B, b_set_time, b_set_pg_fs, b_power_save, b_led, b_set_sdp};

static fake_lte_t FL;
static lte_t LT;
static ring_t RING;
static nvlog_t NS, NC, NCAL;
static flash_ops_t OPS;
static app_t A;

/* 전원을 껐다 켠 것처럼: 플래시 내용은 그대로, 나머지는 새로 */
static void boot(void)
{
    (void)w25q_init(&DEV);                         /* main 처럼: 켤 때 플래시를 깨우고 시작 */
    ring_mount(&RING, &DEV, 0, 4);                 /* 시험용으로 링을 4섹터(512칸)로 */
    nv_w25q_ops(&OPS, &DEV);
    nvlog_mount(&NS, &OPS, PERSIST_SETTINGS_BASE);
    nvlog_mount(&NC, &OPS, PERSIST_COUNTER_BASE);
    nvlog_mount(&NCAL, &OPS, PERSIST_CAL_BASE);    /* main 처럼 app_init 앞 (그 뒤엔 플래시가 잠듦) */
    app_init(&A, &HW, &RING, &NS, &NC, &R);
    app_set_cal(&A, &NCAL);
    app_set_lte(&A, &LT);
}

/* 설정 05 kk vvvvvvvv 만들기 (float32 큰 쪽 먼저) */
static void cal_cmd(uint8_t out[6], uint8_t k, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    out[0] = APP_CFG_CAL;
    out[1] = k;
    out[2] = (uint8_t)(u >> 24);
    out[3] = (uint8_t)(u >> 16);
    out[4] = (uint8_t)(u >> 8);
    out[5] = (uint8_t)u;
}

/* 여러 칸 한 명령: out = 05 + (kk f32)×n, 길이 돌려줌 */
static uint8_t cal_multi(uint8_t *out, const uint8_t *ks, const float *vs, int n)
{
    out[0] = APP_CFG_CAL;
    for (int i = 0; i < n; i++) {
        uint8_t one[6];
        cal_cmd(one, ks[i], vs[i]);
        memcpy(out + 1 + 5 * i, one + 1, 5);
    }
    return (uint8_t)(1 + 5 * n);
}

/* 교정값 보고 시험 벡터 — run.sh 가 파이썬(tools/report_decode.py) · ChirpStack 코덱으로 풀어 맞대 봄 (검수 2차 낮음) */
static FILE *g_vec;
static void dump_cal(void)
{
    if (g_vec == NULL) {
        return;
    }
    uint8_t b[APP_CAL_LEN];
    app_cal_encode(&A, b);
    fprintf(g_vec, "K:");
    for (unsigned i = 0; i < APP_CAL_LEN; i++) {
        fprintf(g_vec, "%02X", b[i]);
    }
    fprintf(g_vec, "|%u", (unsigned)A.cal.has);
    for (unsigned k = 0; k < CAL_N; k++) {
        fprintf(g_vec, "|%.9g", app_cal_value(&A, k));
    }
    fprintf(g_vec, "\n");
}

static const char *last_send(const char *prefix)
{
    const char *hit = NULL;
    for (int i = 0; i < F.nlog; i++) {
        if (strncmp(F.log[i], prefix, strlen(prefix)) == 0) {
            hit = F.log[i];
        }
    }
    return hit;
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

/* 시험이 플래시를 직접 볼 때는 먼저 깨움 — 앱은 일이 끝나면 플래시를 재움 (보드와 같음) */
static void wake(void) { (void)w25q_init(&DEV); }
static ring_err_t t_find_unsent(uint32_t back, uint32_t *seq) { wake(); return ring_find_unsent(&RING, back, seq); }
static ring_err_t t_ring_read(uint32_t seq, ring_rec_t *rec) { wake(); return ring_read(&RING, seq, rec); }
static w25q_err_t t_erase(uint32_t addr) { wake(); return w25q_erase_sector(&DEV, addr); }
static ring_err_t t_ring_append(const uint8_t *rep, uint32_t *seq) { wake(); return ring_append(&RING, rep, seq); }

static bool last_report(report_t *r)
{
    ring_rec_t rec;
    wake();
    return ring_last(&RING, &rec) == RING_OK && report_decode(rec.report, RING_REPORT_SIZE, r);
}

static const uint8_t DEV_EUI[8] = {0xAC, 0x1F, 0x09, 0xFF, 0xFE, 0x0A, 0x1B, 0x2C};
static const uint8_t APP_EUI[8] = {0, 0, 0, 0, 0, 0, 0, 1};
static const uint8_t APP_KEY[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

int main(int argc, char **argv)
{
    g_vec = argc > 1 ? fopen(argv[1], "w") : NULL;
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
    ok("세 개 모두 아직 안 보냄", t_find_unsent(60, &seq) == RING_OK && seq == 0);
    {
        ring_rec_t rec0;
        report_t r0;
        ok("켠 뒤 첫 기록에만 '켬' 비트 (이상 재시작 비트는 없음)",
           t_ring_read(0, &rec0) == RING_OK && report_decode(rec0.report, RING_REPORT_SIZE, &r0) &&
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
    /* 끝에 막 보내기 시작한 기록이 있으면 송신 끝까지 (11/24: 0.8 Pa 치우침이 C(Re) 로는 조리로 안 잡혀 10 s 간격이 됨) */
    while (B.now < until || (R.tx_pending && B.now < until + 60u)) {
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
    ok("최근 기록 중 못 보낸 것 없음", t_find_unsent(60, &seq) == RING_E_MISSING);
    int sends = 0, len_ok = 0;
    for (int i = 0; i < F.nlog; i++) {
        if (strncmp(F.log[i], "AT+SEND=1:", 10) == 0) {
            sends++;
            len_ok += strspn(F.log[i] + 10, "0123456789ABCDEFabcdef") == 2u * REPORT_LEN;
        }
    }
    ok("포트 1 · 28바이트(56자, 보고 v2)로 보냄", sends >= 1 && len_ok == sends);

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
    t_erase(PERSIST_COUNTER_BASE);
    t_erase(PERSIST_COUNTER_BASE + W25Q_SECTOR);
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
        t_ring_append(bytes, &rseq);           /* 누적 저장은 안 함 = 그 사이 끊김 */
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
        t_ring_append(bytes, &rseq);
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
           t_ring_read(RING.next_seq - 2u, &rb) == RING_OK && report_decode(rb.report, RING_REPORT_SIZE, &r1) &&
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
    ok("못 보낸 기록은 링에 남아 있음", t_find_unsent(60, &seq) == RING_OK);

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
        ok("그 뒤 기록들은 모두 '보냄'", t_find_unsent(3, &useq) != RING_OK);
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

    printf("기압을 못 재면 보고 · LED 에 보임 (11/18 검수)\n");
    {
        run_ack(60);
        B.s.patm_held = true;
        led_zero();
        run_ack(60);
        ok("그 분 보고에 BARO_HELD 비트", last_report(&r) && (r.flags & REPORT_F_BARO_HELD));
        ok("LED 빨강 2번 (센서 문제)", B.led_red2 > 0 && B.led_green == 0);
        B.s.patm_held = false;
        run_ack(70);
        led_zero();
        run_ack(60);
        ok("다시 재면 비트 · 빨강 없음", last_report(&r) && !(r.flags & REPORT_F_BARO_HELD) && B.led_red2 == 0);
    }

    printf("1분 기록을 저장 못 하면 LED 빨강 3번\n");
    {
        C.dead = true;
        run_ack(70);                                /* 분 경계를 지나며 저장 실패 */
        led_zero();
        run_ack(20);
        ok("저장 실패 → 빨강 3번 (다른 표시보다 먼저)", A.store_failed && B.led_red3 > 0 && B.led_green == 0);
        C.dead = false;
        run_ack(70);
        led_zero();
        run_ack(30);
        ok("다시 저장되면 빨강 3번 꺼짐", !A.store_failed && B.led_red3 == 0);
    }

    printf("시계를 맞춘 바로 그 걸음에 LTE 결과를 보내도 '소식 없음'으로 안 셈 (11/18 검수)\n");
    {
        run_ack(70);
        const uint32_t real2 = B.now + 2u * 3600u;
        lp_civil_t c2;
        lp_civil_from_unix(real2, &c2);
        snprintf(F.ltime, sizeof(F.ltime), "%02uh%02um%02us on %02u/%02u/%04u", c2.hour, c2.min, c2.sec, c2.month,
                 c2.day, c2.year);
        A.time_synced = false;                      /* 하루가 지난 것처럼 다시 맞추게 */
        A.timereq_on = false;
        const uint32_t syncs0 = A.time_syncs, to0 = A.tx_timeouts;
        bool inj = false;
        const uint32_t stop2 = B.now + 300u;
        while (B.now < stop2 && A.time_syncs == syncs0) {
            if (A.tx_live && !A.tx_is_lte && !A.tx_is_cnt && A.timereq_on && !inj) {   /* 이 송신이 끝나면 시계를 맞춤 */
                A.lte_last.code = LTE_R_NO_SIM;     /* 링 기록을 보내는 동안 LTE 결과가 생김 */
                A.lte_report_pending = true;
                inj = true;
            }
            const uint32_t ms = app_step(&A);
            if (R.tx_pending && !A.tx_is_lte) {
                push(&F, "+EVT:TX_DONE\r\n");
            }
            B.now += ms / 1000u;
            F.clock += ms;
        }
        ok("시계를 2시간 앞으로 맞추고 같은 걸음에 LTE 결과 송신 시작",
           A.time_syncs == syncs0 + 1u && A.tx_live && A.tx_is_lte && B.now >= real2);
        const uint32_t ms = app_step(&A);
        B.now += ms / 1000u;
        F.clock += ms;
        ok("다음 걸음에 '송신 끝 소식 없음'으로 안 셈 (전엔 바로 셈)", A.tx_timeouts == to0 && A.tx_live);
        push(&F, "+EVT:TX_DONE\r\n");
        run_ack(60);
    }

    printf("셀이 낮아 쉬러 가면 마지막 보고만 보내고 밀린 기록은 안 보냄 (11/18 검수)\n");
    {
        run_for(3 * 60);                            /* 송신 끝 소식을 안 줌 → 링에 못 보낸 기록이 쌓임 */
        uint32_t unsent;
        ok("못 보낸 기록이 있음", t_find_unsent(APP_RESEND_WINDOW, &unsent) == RING_OK);
        B.batt = 3240;
        bool seen = false;
        uint32_t s0 = 0, r0 = 0, t0 = 0;
        const uint32_t end = B.now + 3u * 60u + 240u;
        while (B.now < end) {
            const uint32_t ms = app_step(&A);
            if (R.tx_pending) {
                push(&F, "+EVT:TX_DONE\r\n");
            }
            B.now += ms / 1000u;
            F.clock += ms;
            if (A.batt_off && !seen) {
                seen = true;
                s0 = A.sent;
                r0 = A.resends;
                t0 = B.now;
            }
        }
        ok("쉬러 감", seen && A.batt_off && B.now - t0 >= 200u);
        ok("쉬는 동안 다시 보냄 0번 · 송신은 마지막 보고 하나", A.resends == r0 && A.sent - s0 <= 1u && !A.tx_live);
        B.batt = 3700;
    }

    printf("1분마다 로그에 값 한 줄 (보드 오면 2 · 4 · 7단계)\n");
    {
        B.batt = 3700;
        idle_gas();
        run_ack(11 * 60);                           /* 앞 시험의 쉼에서 다시 시작할 때까지 */
        const int n0 = B.minute_logs;
        run_ack(60);
        ok("1분에 한 줄", B.minute_logs == n0 + 1);
        {
            const uint16_t keep = A.counter.report_seq;
            const double keep_tot = A.meter.total_nm3;
            A.counter.report_seq = 54321u;
            A.meter.total_nm3 = 1234.5678;
            B.s.patm_pa = 115535.0;
            run_ack(60);
            const size_t L = strlen(B.minute_log);
            const char *bits = strstr(B.minute_log, "비트 ");
            ok("값이 다 커도 줄 끝(SDP810 · DS18B20 · 영점 칸)까지 안 잘림",
               L > 8u && strstr(B.minute_log, "#54321") != NULL && strstr(B.minute_log, "누적 1234.") != NULL &&
                   bits != NULL && strspn(bits + strlen("비트 "), "0123456789ABCDEF") == 4u &&
                   strstr(B.minute_log, " · SDP810 ") != NULL && strstr(B.minute_log, " · DS18B20 ") != NULL &&
                   strcmp(B.minute_log + L - 3, " Pa") == 0 && strstr(B.minute_log, " · 영점 ") != NULL);
            A.counter.report_seq = keep;
            A.meter.total_nm3 = keep_tot;
            idle_gas();
            run_ack(2 * 60);                        /* 바뀐 기압이 섞이지 않은 분까지 */
        }
        ok("값이 들어 있음 (정압 600 · 기압 101000 · 셀 3700)",
           strstr(B.minute_log, "정압 600 Pa") != NULL && strstr(B.minute_log, "기압 101000 Pa") != NULL &&
               strstr(B.minute_log, "셀 3700 mV") != NULL && strstr(B.minute_log, "누적") != NULL);
    }

    printf("셀 전압 1점 보정 (보드 오면 4단계)\n");
    {
        B.batt = 3600;                              /* 보드가 잰 값 */
        ok("멀티미터 3650 mV → 보정 +50 저장", app_batt_cal(&A, 3650) && A.settings.batt_cal_mv == 50 &&
                                                      b_batt(&B) == 3650u);
        boot();
        ok("다시 켜도 보정값 유지", A.settings.batt_cal_mv == 50);
        ok("다시 맞추면 보정값을 고쳐 씀 (3640 → +40)", app_batt_cal(&A, 3640) && A.settings.batt_cal_mv == 40);
        ok("차이가 300 mV 넘으면 안 받음", !app_batt_cal(&A, 4000) && A.settings.batt_cal_mv == 40);
        ok("실제 값이 말이 안 되면 안 받음", !app_batt_cal(&A, 1500) && A.settings.batt_cal_mv == 40);
        B.batt = 0;
        ok("셀을 못 재면 안 함", !app_batt_cal(&A, 3600) && A.settings.batt_cal_mv == 40);
        B.batt = 3600;
        ok("되돌리기 (3600 → 0)", app_batt_cal(&A, 3600) && A.settings.batt_cal_mv == 0);
        ok("빌드 옵션용: 이미 넣었으면 다시 안 맞춤", A.settings.batt_cal_set && !app_batt_cal_once(&A, 3650) &&
                                                     A.settings.batt_cal_mv == 0);
        B.batt = 3300;                              /* 셀이 실제로 낮아짐 */
        boot();
        ok("그 판을 그대로 다시 켜도 낮아진 셀을 덮지 않음", !app_batt_cal_once(&A, 3650) && b_batt(&B) == 3300u);
        A.settings.batt_cal_set = false;
        B.batt = 3600;
        ok("아직 안 넣었으면 한 번 맞춤", app_batt_cal_once(&A, 3610) && A.settings.batt_cal_mv == 10 &&
                                            A.settings.batt_cal_set);
        ok("되돌리기", app_batt_cal(&A, 3600) && A.settings.batt_cal_mv == 0);
    }

    printf("영점 따라가기 한도에 닿으면 로그 (검수 7번)\n");
    {
        idle_gas();
        meter_set_zero(&A.meter, 0.0);
        B.s.dp2_ok = true;
        B.s.dp2_pa = 0.0;                          /* SDP810 관이 빠져 0 */
        for (int k = 1; k <= 18; k++) {            /* 3시간에 걸쳐 HSC 0 → 2 Pa (매 순간은 조리 문턱 아래) */
            B.s.dp_raw_pa = 2.0 * k / 18.0;
            run_ack(10 * 60);
        }
        ok("영점은 +1 Pa 에서 멈춤 · 앱이 알림 (알린 횟수 = 닿은 횟수)",
           fabs(A.meter.zero_pa - 1.0) < 1e-9 && A.meter.zero_limit_hits > 0u &&
               A.zero_hits_said == A.meter.zero_limit_hits);
        B.s.dp2_ok = false;
        idle_gas();
        meter_set_zero(&A.meter, A.settings.zero_pa);
    }

    printf("SDP810 끄기 · 켜기 (설정 04, 11/24)\n");
    {
        ok("처음 켤 때 앱이 켬으로 알려 줌", B.sdp_on && !A.settings.sdp_off);
        const uint8_t bad[2] = {APP_CFG_SDP, 0x02};
        const uint8_t bad_len[1] = {APP_CFG_SDP};
        ok("02 · 길이 틀림은 무시", !app_downlink(&A, APP_CFG_PORT, bad, 2) && !app_downlink(&A, APP_CFG_PORT, bad_len, 1) &&
                                     B.sdp_on);
        const uint8_t off[2] = {APP_CFG_SDP, 0x00};
        uint32_t saves = NS.next_seq;
        ok("04 00 → 끔 · 저장", app_downlink(&A, APP_CFG_PORT, off, 2) && !B.sdp_on && A.settings.sdp_off &&
                                  NS.next_seq == saves + 1u);
        saves = NS.next_seq;
        ok("같은 값을 또 받으면 다시 저장하지 않음", app_downlink(&A, APP_CFG_PORT, off, 2) && NS.next_seq == saves);
        B.sdp_on = true;
        boot();
        ok("다시 켜도 꺼진 그대로", !B.sdp_on && A.settings.sdp_off);
        const uint8_t on[2] = {APP_CFG_SDP, 0x01};
        ok("04 01 → 켬", app_downlink(&A, APP_CFG_PORT, on, 2) && B.sdp_on && !A.settings.sdp_off);
        boot();
        ok("다시 켜면 켜진 그대로 · 다른 설정(정압 만점) 그대로", B.sdp_on && B.pg_fs == (double)app_pg_fs_pa(&A));
    }

    printf("교정값 (설정 05, 결과는 포트 4 — 11/24 검수 2번)\n");
    {
        ok("처음엔 넣은 칸 없음 · 기본값(목 4 mm, a 0.97739)", A.cal.has == 0u && A.meter.cfg.throat_d_m == 4.0e-3 &&
                                                              app_cal_value(&A, CAL_C_RE_A) == 0.97739);
        dump_cal();
        uint8_t c6[6];
        cal_cmd(c6, 1, 2.0f);
        ok("a 2.0 은 범위 밖 → 무시", !app_downlink(&A, APP_CFG_PORT, c6, 6) && A.cal.has == 0u);
        cal_cmd(c6, 2, -1000.0f);
        ok("b −1000 은 Re 최소에서 C 0.40 → 무시", !app_downlink(&A, APP_CFG_PORT, c6, 6) && A.cal.has == 0u);
        cal_cmd(c6, 5, 4.5f);
        ok("입구 4.5 mm 는 β 0.89 → 무시", !app_downlink(&A, APP_CFG_PORT, c6, 6) && A.cal.has == 0u);
        cal_cmd(c6, 9, 1.0f);
        ok("없는 칸 09 → 무시", !app_downlink(&A, APP_CFG_PORT, c6, 6));
        const uint8_t bad_len[3] = {APP_CFG_CAL, 0x01, 0x3F};
        ok("길이 틀림 → 무시", !app_downlink(&A, APP_CFG_PORT, bad_len, 3));

        B.batt = 3700;
        app_lora_start(&A);                        /* 앞 시험이 다시 켜서(boot) LoRa 가 안 켜진 상태 — 켜고 가입 */
        push(&F, "+EVT:JOINED\r\n");
        F.joined = true;
        run_ack(2 * 60);                           /* 밀린 1분 보고부터 보냄 */
        F.nlog = 0;
        const uint32_t saves = NCAL.next_seq;
        cal_cmd(c6, 4, 4.1f);
        ok("05 04 4.1 → 목 4.1 mm 바로 적용 · 저장",
           app_downlink(&A, APP_CFG_PORT, c6, 6) && fabs(A.meter.cfg.throat_d_m - 4.1e-3) < 1e-9 && A.cal.has == 0x08u &&
               NCAL.next_seq == saves + 1u && A.cal_report_pending);
        ok("다른 칸은 기본값 그대로", A.meter.cfg.inlet_d_m == 16.0e-3 && A.meter.cfg.c_re_a == 0.97739);
        dump_cal();
        run_ack(60);
        const char *s4 = last_send("AT+SEND=4:");
        ok("포트 4 로 34 B 보고 한 번 · 형식 01 · 넣은 칸 08 · 목 칸 = 4.1 (40833333)",
           count_writes("AT+SEND=4:") == 1 && !A.cal_report_pending && s4 != NULL && strlen(s4) == 10u + 68u &&
               strncmp(s4 + 10, "0108", 4) == 0 && strncmp(s4 + 10 + 4 + 3 * 8, "40833333", 8) == 0);
        ok("교정값 보고는 링 기록이 아님 (다음 1분 보고 · 보냄 표시는 그대로)", !A.tx_is_cal);

        const uint32_t s1 = NCAL.next_seq;
        ok("같은 값 → 저장 안 함 · 보고는 다시", app_downlink(&A, APP_CFG_PORT, c6, 6) && NCAL.next_seq == s1 &&
                                                     A.cal_report_pending);
        run_ack(60);
        boot();
        ok("다시 켜도 목 4.1 mm", fabs(A.meter.cfg.throat_d_m - 4.1e-3) < 1e-9 && A.cal.has == 0x08u);
        cal_cmd(c6, 1, 0.0f);
        ok("a 0 → C(Re) 끔 (고정 C)", app_downlink(&A, APP_CFG_PORT, c6, 6) && A.meter.cfg.c_re_a == 0.0 &&
                                         meter_discharge_c(&A.meter.cfg, 50.0, 1.0, 298.15, NULL) == 0.97);
        const uint8_t ask[2] = {APP_CFG_CAL, 0xFF};
        A.cal_report_pending = false;
        const uint32_t s2 = NCAL.next_seq;
        ok("05 FF → 보고만 (저장 안 함)", app_downlink(&A, APP_CFG_PORT, ask, 2) && A.cal_report_pending &&
                                            NCAL.next_seq == s2);
        printf("교정값 여러 칸을 한 명령에 (검수 2차 중간)\n");
        {
            uint8_t mc[41];
            const uint8_t k1[1] = {2};
            const float v1[1] = {-1500.0f};
            cal_cmd(c6, 1, 0.97739f);
            (void)app_downlink(&A, APP_CFG_PORT, c6, 6);   /* a 다시 켬 */
            const uint8_t has0 = A.cal.has;
            ok("b −1500 혼자는 Re 최소 1725 에서 C 0.11 → 무시",
               !app_downlink(&A, APP_CFG_PORT, mc, cal_multi(mc, k1, v1, 1)) && A.cal.has == has0);
            const uint8_t k3[3] = {1, 2, 3};
            const float v3[3] = {0.97739f, -1500.0f, 5000.0f};
            ok("a · b · Re 최소 5000 을 한 명령에 → C 0.68–0.98 로 받음 · 한 번에 적용",
               app_downlink(&A, APP_CFG_PORT, mc, cal_multi(mc, k3, v3, 3)) && A.meter.cfg.c_re_b == -1500.0 &&
                   A.meter.cfg.c_re_min == 5000.0 && (A.cal.has & 0x07u) == 0x07u);
            dump_cal();
            const uint8_t kb[3] = {4, 9, 5};
            const float vb[3] = {4.2f, 1.0f, 18.0f};
            const double th = A.meter.cfg.throat_d_m;
            ok("중간에 없는 칸(09)이 있으면 명령 전체 무시 (앞 칸도 안 바뀜)",
               !app_downlink(&A, APP_CFG_PORT, mc, cal_multi(mc, kb, vb, 3)) && A.meter.cfg.throat_d_m == th);
            const uint8_t k7[1] = {0x83};
            const float v7[1] = {0.0f};
            const uint8_t k8[2] = {0x82, 0x83};
            const float v8[2] = {0.0f, 0.0f};
            ok("83 → Re 최소만 기본값 … 그러면 b −1500 이 1725 에서 C 0.11 이라 무시",
               !app_downlink(&A, APP_CFG_PORT, mc, cal_multi(mc, k7, v7, 1)) && A.meter.cfg.c_re_min == 5000.0);
            ok("82 · 83 같이 → b · Re 최소 기본값, 목 4.1 mm 는 그대로",
               app_downlink(&A, APP_CFG_PORT, mc, cal_multi(mc, k8, v8, 2)) && A.meter.cfg.c_re_b == -158.91 &&
                   A.meter.cfg.c_re_min == 1725.0 && (A.cal.has & 0x06u) == 0u && fabs(A.meter.cfg.throat_d_m - 4.1e-3) < 1e-9);
            const uint8_t bad7[7] = {APP_CFG_CAL, 0x04, 0x40, 0x83, 0x33, 0x33, 0x01};
            ok("길이가 1 + 5n 이 아니면 무시", !app_downlink(&A, APP_CFG_PORT, bad7, 7));
            boot();
            ok("다시 켜도 그대로 (a · 목 넣음, b · Re 최소 기본값)", (A.cal.has & 0x0Fu) == 0x09u &&
                                                                     fabs(A.meter.cfg.throat_d_m - 4.1e-3) < 1e-9 &&
                                                                     A.meter.cfg.c_re_b == -158.91);
        }
        const uint8_t reset[2] = {APP_CFG_CAL, 0x00};
        ok("05 00 → 모두 기본값", app_downlink(&A, APP_CFG_PORT, reset, 2) && A.cal.has == 0u &&
                                     A.meter.cfg.throat_d_m == 4.0e-3 && A.meter.cfg.c_re_a == 0.97739);
        run_ack(60);
        boot();
        ok("다시 켜도 기본값", A.cal.has == 0u && A.meter.cfg.throat_d_m == 4.0e-3);
    }

    printf("방출 기준(물트랩 압력)을 설정 명령으로 넣기\n");
    {
        B.batt = 3700;
        B.s.dp_raw_pa = 0.0;
        B.s.pg_pa = 960.0;                          /* 트랩이 물 10 cm 쯤인 소화조: 정압이 여기서 멈춤 */
        ok("처음엔 기본값 물 20 cm (1962 Pa)", app_vent_p_pa(&A) == 98.1 * 20.0 && A.meter.cfg.vent_p_pa == 98.1 * 20.0);
        const uint32_t v0 = A.meter.vent_minutes;
        run_ack(15 * 60);
        ok("기본값으로는 960 Pa 에서 방출을 못 잡음", A.meter.vent_minutes == v0);

        const uint8_t low[3] = {APP_CFG_VENT_P, 0x00, 0x32};       /* 50 Pa */
        const uint8_t high[3] = {APP_CFG_VENT_P, 0x1B, 0x58};      /* 7000 Pa > 센서 만점 6000 */
        const uint8_t bad_len[2] = {APP_CFG_VENT_P, 0x03};
        ok("범위 밖(50 · 7000 Pa) · 길이 틀림은 무시", !app_downlink(&A, APP_CFG_PORT, low, 3) &&
                                                       !app_downlink(&A, APP_CFG_PORT, high, 3) &&
                                                       !app_downlink(&A, APP_CFG_PORT, bad_len, 2) &&
                                                       A.meter.cfg.vent_p_pa == 98.1 * 20.0);
        const uint8_t set[3] = {APP_CFG_VENT_P, 0x03, 0xD5};       /* 981 Pa = 물 10 cm */
        ok("03 03 D5 → 981 Pa 로 바로 적용", app_downlink(&A, APP_CFG_PORT, set, 3) && A.meter.cfg.vent_p_pa == 981.0 &&
                                               A.settings.vent_p_pa == 981.0);
        const uint32_t saves = NS.next_seq;
        ok("같은 값을 또 받으면 다시 저장하지 않음", app_downlink(&A, APP_CFG_PORT, set, 3) && NS.next_seq == saves);
        const uint32_t v1 = A.meter.vent_minutes;
        run_ack(15 * 60);
        ok("이제 960 Pa 에서 평평하면 방출로 셈 · 보고에 방출 비트", A.meter.vent_minutes > v1 && last_report(&r) &&
                                                                    (r.flags & REPORT_F_VENTING));
        boot();
        ok("다시 켜도 981 Pa 유지", A.meter.cfg.vent_p_pa == 981.0 && app_vent_p_pa(&A) == 981.0);
        const uint8_t back[3] = {APP_CFG_VENT_P, 0x00, 0x00};
        ok("0 을 보내면 기본값으로", app_downlink(&A, APP_CFG_PORT, back, 3) && A.meter.cfg.vent_p_pa == 98.1 * 20.0);
        idle_gas();
    }

    printf("횟수 보고 (포트 3, 11/18)\n");
    {
        idle_gas();
        B.batt = 3700;
        F.ltime[0] = '\0';                         /* 가짜 망 시각은 고정이라 이미 지난 시각 — 시계를 되돌리지 않게 비움 */
        app_lora_start(&A);
        push(&F, "+EVT:JOINED\r\n");
        F.joined = true;
        run_ack(3 * 60);
        ok("가입 뒤 한 번 보냄 · 지금 횟수와 같음",
           A.cnt_reports == 1u && A.cnt_sent.cooking == A.meter.events.count &&
               A.cnt_sent.depletions == A.meter.depletions && A.cnt_sent.vent_minutes == A.meter.vent_minutes &&
               A.cnt_sent.leaks == A.meter.leaks &&
               (uint16_t)(A.counter.report_seq - 1u - A.cnt_sent.seq) <= 3u);
        const uint32_t n1 = A.cnt_reports;
        run_ack(60 * 60);
        ok("횟수가 안 바뀌면 안 보냄 (1시간)", A.cnt_reports == n1);

        const uint32_t ev0 = A.meter.events.count;
        B.s.dp_raw_pa = 40.8;                       /* 조리 3분 */
        run_ack(3 * 60);
        B.s.dp_raw_pa = 0.0;
        run_ack(3 * 60);
        ok("조리 한 번 끝남", A.meter.events.count == ev0 + 1u);
        run_ack(11 * 60);
        ok("10분 안에 횟수 보고 · 조리 +1", A.cnt_reports == n1 + 1u && A.cnt_sent.cooking == ev0 + 1u);

        const uint32_t n2 = A.cnt_reports;
        run_ack(30 * 60);
        ok("그 뒤로 안 바뀌면 다시 안 보냄", A.cnt_reports == n2);
        A.cnt_last_s -= APP_CNT_DAY_S;              /* 마지막으로 보낸 게 하루 전인 것처럼 */
        run_ack(11 * 60);
        ok("안 바뀌어도 하루에 한 번", A.cnt_reports == n2 + 1u);

        A.meter.leaks = 3u;
        run_ack(70);
        boot();
        ok("누설 횟수도 저장 · 다시 켜도 유지", A.meter.leaks == 3u);

        printf("횟수 보고 고친 것 (11/18 6차 검수)\n");
        app_lora_start(&A);
        push(&F, "+EVT:JOINED\r\n");
        F.joined = true;
        run_ack(3 * 60);
        /* 방출 중: 방출 분이 매분 늘어도 그것만으로는 안 보냄 */
        const uint8_t vset[3] = {APP_CFG_VENT_P, 0x03, 0xD5};
        app_downlink(&A, APP_CFG_PORT, vset, 3);
        B.s.pg_pa = 960.0;
        const uint32_t nv = A.cnt_reports, v0 = A.meter.vent_minutes;
        run_ack(40 * 60);
        ok("방출 중 방출 분은 늘지만 횟수 보고는 안 나감", A.meter.vent_minutes > v0 + 20u && A.cnt_reports == nv);
        const uint8_t vback[3] = {APP_CFG_VENT_P, 0x00, 0x00};
        app_downlink(&A, APP_CFG_PORT, vback, 3);
        idle_gas();
        run_ack(2 * 60);

        /* 누설 판정 중에 다시 켜져도 같은 누설을 또 세지 않음 */
        B.s.dp_raw_pa = A.meter.zero_pa + 0.35;     /* 누설 문턱 0.3 Pa 위, 조리 문턱 아래 */
        const uint32_t l0 = A.meter.leaks;
        run_ack(2u * 3600u + 20u * 60u);
        ok("작은 차압 2시간 → 누설 한 번", A.meter.leak_active && A.meter.leaks == l0 + 1u);
        boot();
        ok("다시 켜도 누설 판정 중 그대로", A.meter.leak_active && A.meter.leaks == l0 + 1u);
        run_ack(2u * 3600u + 20u * 60u);
        ok("같은 누설을 또 세지 않음", A.meter.leak_active && A.meter.leaks == l0 + 1u);
        idle_gas();
        run_ack(40 * 60);

        /* 새 계량기: 1분 보고를 만들기 전엔 횟수 보고를 안 보냄 */
        app_lora_start(&A);                         /* 위에서 다시 켰으니 LoRa 도 다시 */
        push(&F, "+EVT:JOINED\r\n");
        F.joined = true;
        run_ack(3 * 60);
        A.reports = 0u;
        A.counter.report_seq = 0u;
        A.cnt_sent_valid = false;
        A.next_cnt_try_s = 0u;
        const uint32_t nn = A.cnt_reports;
        B.now += 60u - (B.now % 60u) + 5u;          /* 분 경계 5초 뒤로 */
        A.last_s = B.now;
        A.last_minute = B.now / 60u;
        run_ack(40);
        ok("첫 1분 보고 전엔 안 보냄", A.cnt_reports == nn);
        run_ack(2 * 60);
        ok("첫 1분 보고 뒤 보냄 · 보고 번호 칸 0 (65535 아님)", A.cnt_reports == nn + 1u && A.cnt_sent.seq == 0u);
    }

    printf("\n%d / %d 통과\n", checks - fails, checks);
    if (g_vec != NULL) {
        fclose(g_vec);
    }
    return fails ? 1 : 0;
}
