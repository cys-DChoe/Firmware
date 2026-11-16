/* LoRa 모듈(RAK3172 · RUI3 AT) 맥 시험 — 매뉴얼 형식대로 답하는 가짜 모듈을 붙여 확인
 *   답 형식: <CR><LF>값<CR><LF><CR><LF>상태<CR><LF>, 질의 값 줄은 "AT+NJS=1" 처럼 명령 이름을 되풀이
 *   이벤트: +EVT:JOINED · +EVT:TX_DONE · +EVT:RX_1:-70:8:UNICAST:1:1234 (매뉴얼 예)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rak3172.h"

static int fails = 0, checks = 0;

static void ok(const char *what, bool v)
{
    checks++;
    if (!v) {
        fails++;
    }
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

/* ---------- 가짜 모듈 ---------- */

#define NKEY 16
typedef struct {
    char rxq[16384];
    size_t head, tail;
    uint32_t clock;
    char txline[RAK_LINE_MAX + 8];
    size_t txlen;
    /* 모듈 상태 */
    const char *keys[NKEY];
    char vals[NKEY][80];
    int nkeys;
    bool echo, asleep, dead, joined;
    uint32_t awake_until;      /* LPM 켜짐: 이 시각이 지나면 잠 */
    int busy_left;
    const char *inject;        /* 다음 답의 값 줄과 OK 사이에 끼울 줄 */
    char log[64][RAK_LINE_MAX];   /* 받은 명령 */
    int nlog;
    int resets;
    bool rst_level;
    uint32_t rst_low_ms;
} fake_t;

static void push(fake_t *f, const char *s)
{
    size_t n = strlen(s);
    memcpy(f->rxq + f->tail, s, n);
    f->tail += n;
}

static void pushf(fake_t *f, const char *a, const char *b)
{
    push(f, a);
    push(f, b);
}

static char *val_of(fake_t *f, const char *key)
{
    for (int i = 0; i < f->nkeys; i++) {
        if (strcmp(f->keys[i], key) == 0) {
            return f->vals[i];
        }
    }
    return NULL;
}

static void set_key(fake_t *f, const char *key, const char *v)
{
    char *p = val_of(f, key);
    if (p == NULL) {
        f->keys[f->nkeys] = key;
        p = f->vals[f->nkeys++];
    }
    strncpy(p, v, 79);
}

static void status(fake_t *f, const char *st) { pushf(f, "\r\n", st); push(f, "\r\n"); }

static void banner(fake_t *f)
{
    push(f, "RAKwireless RAK3172\r\n------------------------------------------------------\r\n"
            "Version: RUI_4.0.4_RAK3172-E\r\nCurrent Work Mode: LoRaWAN.\r\n");
}

/* 모듈 기본값(공장 출하) — 매뉴얼 기본: NWM 1 · NJM 1 · BAND 4 · CFM 0 · ADR 1 */
static void fake_factory(fake_t *f)
{
    memset(f, 0, sizeof(*f));
    f->rst_level = true;
    static const char *K[] = {"AT+NWM", "AT+NJM", "AT+BAND", "AT+DEVEUI", "AT+APPEUI", "AT+APPKEY", "AT+CFM",
                              "AT+ADR", "AT+LBT", "AT+LBTRSSI", "AT+LBTSCANTIME", "AT+LPMLVL", "AT+LPM", "AT+NJS"};
    static const char *V[] = {"1", "1", "4", "AC1F09FFFE000000", "0000000000000000",
                              "00000000000000000000000000000000", "0", "1", "0", "-80", "5", "1", "0", "0"};
    for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++) {
        set_key(f, K[i], V[i]);
    }
}

static void handle(fake_t *f, const char *cmd)
{
    if (f->dead) {
        return;
    }
    const bool lpm = strcmp(val_of(f, "AT+LPM"), "1") == 0;
    if (f->asleep || (lpm && f->clock > f->awake_until)) {
        /* 잠에서 깨는 데 첫 줄을 씀 → 답 없음 (최악으로 가정), 깬 뒤 1 s 는 깨어 있음 */
        f->asleep = false;
        f->awake_until = f->clock + 1000u;
        return;
    }
    if (f->nlog < 64) {
        strcpy(f->log[f->nlog++], cmd);
    }
    if (f->echo) {
        pushf(f, cmd, "\r\n");
    }
    const char *eq = strchr(cmd, '=');
    if (strcmp(cmd, "AT") == 0) {
        status(f, "OK");
    } else if (eq && strcmp(eq, "=?") == 0) {
        char key[40];
        snprintf(key, sizeof(key), "%.*s", (int)(eq - cmd), cmd);
        const char *v = val_of(f, key);
        if (v == NULL) {
            status(f, "AT_ERROR");
        } else {
            push(f, "\r\n");
            pushf(f, key, "=");
            pushf(f, v, "\r\n");
            if (f->inject) {
                pushf(f, f->inject, "\r\n");
                f->inject = NULL;
            }
            status(f, "OK");
        }
    } else if (strncmp(cmd, "AT+SEND=", 8) == 0) {
        if (f->busy_left > 0) {
            f->busy_left--;
            status(f, "AT_BUSY_ERROR");
        } else if (!f->joined) {
            status(f, "AT_NO_NETWORK_JOINED");
        } else {
            status(f, "OK");
        }
    } else if (strncmp(cmd, "AT+JOIN=", 8) == 0) {
        status(f, "OK");
    } else if (strncmp(cmd, "AT+BAUD=", 8) == 0) {
        status(f, "OK");
    } else if (eq) {
        char key[40];
        snprintf(key, sizeof(key), "%.*s", (int)(eq - cmd), cmd);
        if (val_of(f, key) == NULL || strcmp(key, "AT+NJS") == 0) {
            status(f, "AT_PARAM_ERROR");
        } else {
            set_key(f, key, eq + 1);
            status(f, "OK");
            if (strcmp(key, "AT+NWM") == 0) {
                banner(f);      /* 모드 바꾸면 다시 켜짐 */
            }
        }
    } else {
        status(f, "AT_ERROR");
    }
    /* LPM: 답한 뒤 50 ms 지나면 잠 */
    f->awake_until = f->clock + 50u;
}

static int fk_write(void *ctx, const char *s, size_t n)
{
    fake_t *f = ctx;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            if (f->txlen > 0 && f->txline[f->txlen - 1] == '\r') {
                f->txlen--;
            }
            f->txline[f->txlen] = '\0';
            handle(f, f->txline);
            f->txlen = 0;
        } else if (f->txlen < sizeof(f->txline) - 1) {
            f->txline[f->txlen++] = s[i];
        }
    }
    return 0;
}

static int fk_getc(void *ctx)
{
    fake_t *f = ctx;
    if (f->head == f->tail) {
        f->clock++;                 /* 받을 게 없으면 1 ms 흐름 */
        return -1;
    }
    return (unsigned char)f->rxq[f->head++];
}

static uint32_t fk_ms(void *ctx) { return ((fake_t *)ctx)->clock; }
static void fk_delay(void *ctx, uint32_t ms) { ((fake_t *)ctx)->clock += ms; }

static void fk_rst(void *ctx, bool level)
{
    fake_t *f = ctx;
    if (!level) {
        f->rst_low_ms = f->clock;
    } else if (!f->rst_level) {
        f->rst_low_ms = f->clock - f->rst_low_ms;
        f->resets++;
        f->asleep = false;
        banner(f);
    }
    f->rst_level = level;
}

static fake_t F;
static rak_t R;

static void fresh(void)
{
    fake_factory(&F);
    rak_io_t io = {fk_write, fk_getc, fk_ms, fk_delay, fk_rst, &F};
    rak_init(&R, &io);
}

static int count_writes(const char *prefix)
{
    int n = 0;
    for (int i = 0; i < F.nlog; i++) {
        if (strncmp(F.log[i], prefix, strlen(prefix)) == 0 && strstr(F.log[i], "=?") == NULL) {
            n++;
        }
    }
    return n;
}

/* ---------- 시험 ---------- */

static void test_parse(void)
{
    printf("상태 줄 풀기 (매뉴얼 목록)\n");
    bool st;
    ok("OK", rak_parse_status("OK", &st) == RAK_OK && st);
    ok("AT_PARAM_ERROR", rak_parse_status("AT_PARAM_ERROR", &st) == RAK_E_PARAM && st);
    ok("AT_BUSY_ERROR", rak_parse_status("AT_BUSY_ERROR", &st) == RAK_E_BUSY && st);
    ok("AT_NO_NETWORK_JOINED", rak_parse_status("AT_NO_NETWORK_JOINED", &st) == RAK_E_NOT_JOINED && st);
    ok("AT_ERROR · AT_RX_ERROR · COMMAND_LOCKED → 오류",
       rak_parse_status("AT_ERROR", &st) == RAK_E_AT && rak_parse_status("AT_RX_ERROR", &st) == RAK_E_AT &&
           rak_parse_status("COMMAND_LOCKED", &st) == RAK_E_AT);
    rak_parse_status("AT+NJS=1", &st);
    ok("값 줄 AT+NJS=1 은 상태 아님", !st);

    printf("이벤트 줄 풀기\n");
    rak_event_t e;
    ok("+EVT:JOINED", rak_parse_event("+EVT:JOINED", &e) && e.kind == RAK_EV_JOINED);
    ok("+EVT:JOIN_FAILED_RX_TIMEOUT", rak_parse_event("+EVT:JOIN_FAILED_RX_TIMEOUT", &e) && e.kind == RAK_EV_JOIN_FAILED);
    ok("+EVT:JOIN FAILED (매뉴얼 예의 띄어쓰기)", rak_parse_event("+EVT:JOIN FAILED", &e) && e.kind == RAK_EV_JOIN_FAILED);
    ok("+EVT:TX_DONE", rak_parse_event("+EVT:TX_DONE", &e) && e.kind == RAK_EV_TX_DONE);
    ok("+EVT: SEND CONFIRMED OK (띄어쓰기)", rak_parse_event("+EVT: SEND CONFIRMED OK", &e) && e.kind == RAK_EV_CONFIRMED_OK);
    ok("+EVT:SEND_CONFIRMED_FAILED", rak_parse_event("+EVT:SEND_CONFIRMED_FAILED", &e) && e.kind == RAK_EV_CONFIRMED_FAILED);
    ok("+EVT:RX_1:-70:8:UNICAST:1:1234 (매뉴얼 예)",
       rak_parse_event("+EVT:RX_1:-70:8:UNICAST:1:1234", &e) && e.kind == RAK_EV_RX && e.rssi == -70 && e.snr == 8 &&
           e.port == 1 && e.len == 2 && e.data[0] == 0x12 && e.data[1] == 0x34);
    ok("앞머리 없는 RX_1:-105:-3:UNICAST:10:AB 도 받음",
       rak_parse_event("RX_1:-105:-3:UNICAST:10:AB", &e) && e.kind == RAK_EV_RX && e.rssi == -105 && e.snr == -3 &&
           e.port == 10 && e.len == 1 && e.data[0] == 0xAB);
    ok("값 없는 수신(빈 16진)", rak_parse_event("+EVT:RX_1:-90:5:UNICAST:0:", &e) && e.kind == RAK_EV_RX && e.len == 0);
    ok("16진이 깨진 수신은 버림", !rak_parse_event("+EVT:RX_1:-90:5:UNICAST:1:12G4", &e));
    ok("시작 문구는 이벤트 아님", !rak_parse_event("Current Work Mode: LoRaWAN.", &e));

    printf("16진 바꾸기\n");
    char h[16];
    const uint8_t b[] = {0x01, 0xAB, 0xFF};
    ok("01 AB FF → \"01ABFF\"", rak_hex_encode(b, 3, h, sizeof(h)) == 6 && strcmp(h, "01ABFF") == 0);
    uint8_t d[4];
    ok("\"01abff\" → 3바이트", rak_hex_decode("01abff", d, sizeof(d)) == 3 && d[1] == 0xAB);
    ok("홀수 자리는 거부", rak_hex_decode("123", d, sizeof(d)) < 0);
}

static void test_cmd(void)
{
    printf("명령 하나 주고받기\n");
    fresh();
    char v[40];
    ok("AT+BAND=? → \"4\"", rak_cmd(&R, "AT+BAND=?", v, sizeof(v)) == RAK_OK && strcmp(v, "4") == 0);
    ok("보낸 줄 = \"AT+BAND=?\"", F.nlog == 1 && strcmp(F.log[0], "AT+BAND=?") == 0);
    F.echo = true;
    ok("메아리 켜져도 값 \"1\"", rak_cmd(&R, "AT+NWM=?", v, sizeof(v)) == RAK_OK && strcmp(v, "1") == 0);
    F.echo = false;
    F.inject = "+EVT:RX_1:-70:8:UNICAST:1:1234";
    ok("명령 중 끼어든 수신 이벤트도 처리", rak_cmd(&R, "AT+ADR=?", v, sizeof(v)) == RAK_OK && R.rx_new &&
                                                    R.rx.port == 1 && R.rx.len == 2 && strcmp(v, "1") == 0);
    ok("없는 설정 → AT_PARAM_ERROR", rak_cmd(&R, "AT+NJS=1", NULL, 0) == RAK_E_PARAM);
    F.dead = true;
    const uint32_t t0 = F.clock;
    ok("답 없음 → 시간 초과", rak_cmd(&R, "AT", NULL, 0) == RAK_E_TIMEOUT);
    ok("기다린 시간 = 1 s", F.clock - t0 >= RAK_CMD_TIMEOUT_MS && F.clock - t0 < RAK_CMD_TIMEOUT_MS + 5);
}

static rak_cfg_t test_cfg(void)
{
    rak_cfg_t c;
    rak_default_cfg(&c);
    c.dev_eui = "AC1F09FFFE0A1B2C";
    c.app_eui = "0000000000000001";
    c.app_key = "00112233445566778899AABBCCDDEEFF";
    return c;
}

static void test_configure(void)
{
    printf("처음 한 번 설정 (공장값 모듈 → 국내 시험 KR920)\n");
    fresh();
    rak_cfg_t c = test_cfg();
    ok("설정 OK", rak_configure(&R, &c) == RAK_OK);
    ok("BAND 7 (KR920)", strcmp(val_of(&F, "AT+BAND"), "7") == 0);
    ok("키 3개 들어감", strcmp(val_of(&F, "AT+DEVEUI"), c.dev_eui) == 0 &&
                            strcmp(val_of(&F, "AT+APPEUI"), c.app_eui) == 0 && strcmp(val_of(&F, "AT+APPKEY"), c.app_key) == 0);
    ok("LBT 켬 · −65 dBm · 5", strcmp(val_of(&F, "AT+LBT"), "1") == 0 && strcmp(val_of(&F, "AT+LBTRSSI"), "-65") == 0 &&
                                   strcmp(val_of(&F, "AT+LBTSCANTIME"), "5") == 0);
    ok("LPMLVL 2 · LPM 1 (맨 끝)", strcmp(val_of(&F, "AT+LPMLVL"), "2") == 0 && strcmp(val_of(&F, "AT+LPM"), "1") == 0 &&
                                     strncmp(F.log[F.nlog - 1], "AT+LPM=1", 8) == 0);
    ok("이미 같은 값(NWM · NJM · CFM · ADR · LBTSCANTIME)은 안 씀",
       count_writes("AT+NWM") == 0 && count_writes("AT+NJM") == 0 && count_writes("AT+CFM") == 0 &&
           count_writes("AT+ADR") == 0 && count_writes("AT+LBTSCANTIME") == 0);
    const int w1 = (int)R.writes;
    ok("쓴 설정 = 8개 (BAND · 키 3 · LBT · RSSI · LPMLVL · LPM)", w1 == 8);

    printf("다시 설정하면 아무것도 안 씀 (잠든 모듈은 \"AT\" 로 깨움)\n");
    const int before = (int)R.writes;
    ok("설정 OK", rak_configure(&R, &c) == RAK_OK);
    ok("쓴 것 0개", (int)R.writes == before);

    printf("P2P 모드로 와 있던 모듈 → LoRaWAN 으로 바꾸고 다시 켜짐 기다림\n");
    fresh();
    set_key(&F, "AT+NWM", "0");
    const uint32_t t0 = F.clock;
    ok("설정 OK", rak_configure(&R, &c) == RAK_OK);
    ok("NWM=1 씀", strcmp(val_of(&F, "AT+NWM"), "1") == 0 && count_writes("AT+NWM") == 1);
    ok("다시 켜짐 1.5 s 기다림", F.clock - t0 >= RAK_BOOT_MS);

    printf("키가 잘못되면 보내지 않음\n");
    fresh();
    c.app_key = "0011";
    ok("짧은 APPKEY → 인자 오류", rak_configure(&R, &c) == RAK_E_ARG && F.nlog == 0);
}

static void test_join_send(void)
{
    printf("가입 → 1분 보고 보내기 → 송신 끝\n");
    fresh();
    rak_cfg_t c = test_cfg();
    rak_configure(&R, &c);
    ok("가입 시작 OK", rak_join_start(&R) == RAK_OK && R.joining && !R.joined);
    ok("가입 명령 = AT+JOIN=1:0:10:8", strcmp(F.log[F.nlog - 1], "AT+JOIN=1:0:10:8") == 0);
    ok("이벤트 오기 전엔 아직 가입 중", rak_poll(&R) == RAK_EV_NONE && R.joining);
    push(&F, "+EVT:JOINED\r\n");
    F.joined = true;
    ok("+EVT:JOINED → 가입됨", rak_poll(&R) == RAK_EV_JOINED && R.joined && !R.joining);

    const uint8_t rep[24] = {0x01, 0x00, 0x2A, 0x6A, 0x5B, 0x3C, 0x00};
    ok("송신 시작 OK", rak_send_start(&R, 2, rep, sizeof(rep)) == RAK_OK && R.tx_pending);
    ok("보낸 줄 = AT+SEND=2:01002A6A5B3C00… (48자)",
       strncmp(F.log[F.nlog - 1], "AT+SEND=2:01002A6A5B3C00", 24) == 0 && strlen(F.log[F.nlog - 1]) == 10 + 48);
    ok("끝나기 전 또 보내면 BUSY (모듈에 안 보냄)", rak_send_start(&R, 2, rep, sizeof(rep)) == RAK_E_BUSY);
    push(&F, "+EVT:TX_DONE\r\n");
    ok("+EVT:TX_DONE → 송신 끝", rak_poll(&R) == RAK_EV_TX_DONE && !R.tx_pending && R.last_tx == RAK_EV_TX_DONE);

    printf("모듈이 바쁠 때 (듀티 대기 · 수신 창)\n");
    F.busy_left = 1;
    ok("AT_BUSY_ERROR → BUSY, 송신 중 표시 안 함", rak_send_start(&R, 2, rep, 4) == RAK_E_BUSY && !R.tx_pending);
    ok("다음 번엔 OK", rak_send_start(&R, 2, rep, 4) == RAK_OK);
    push(&F, "+EVT:TX_DONE\r\n");
    rak_poll(&R);

    printf("내려온 값\n");
    push(&F, "+EVT:RX_1:-98:-2:UNICAST:3:0A0B\r\n");
    ok("포트 3 · 0A 0B · RSSI −98", rak_poll(&R) == RAK_EV_RX && R.rx_new && R.rx.port == 3 && R.rx.len == 2 &&
                                       R.rx.data[1] == 0x0B && R.rx.rssi == -98);

    printf("망에서 떨어졌을 때\n");
    F.joined = false;
    ok("AT_NO_NETWORK_JOINED → 가입 안 됨으로", rak_send_start(&R, 2, rep, 4) == RAK_E_NOT_JOINED && !R.joined);

    printf("인자 확인\n");
    ok("포트 0 거부", rak_send_start(&R, 0, rep, 4) == RAK_E_ARG);
    ok("포트 234 거부", rak_send_start(&R, 234, rep, 4) == RAK_E_ARG);
    ok("0바이트 거부", rak_send_start(&R, 2, rep, 0) == RAK_E_ARG);
    ok("243바이트 거부", rak_send_start(&R, 2, rep, RAK_PAYLOAD_MAX + 1) == RAK_E_ARG);
}

static void test_reset_baud(void)
{
    printf("리셋 · 속도 바꾸기\n");
    fresh();
    ok("리셋 뒤 \"AT\" 응답 OK", rak_hw_reset(&R) == RAK_OK && F.resets == 1);
    ok("NRST 낮음 2 ms", F.rst_low_ms == RAK_RESET_PULSE_MS);
    ok("시작 문구는 버림 (이벤트 아님)", R.rx_new == false && !R.joined);
    ok("AT+BAUD=9600", rak_set_baud(&R, 9600) == RAK_OK && strcmp(F.log[F.nlog - 1], "AT+BAUD=9600") == 0);

    printf("잠든 모듈 깨우기\n");
    fresh();
    R.lpm = true;
    F.asleep = true;
    char v[8];
    ok("첫 \"AT\" 는 잃어도 두 번째로 깨워 값 읽음", rak_cmd(&R, "AT+BAND=?", v, sizeof(v)) == RAK_OK && strcmp(v, "4") == 0);
    F.dead = true;
    ok("세 번 다 답 없으면 시간 초과", rak_cmd(&R, "AT+BAND=?", v, sizeof(v)) == RAK_E_TIMEOUT);
}

int main(void)
{
    test_parse();
    test_cmd();
    test_configure();
    test_join_send();
    test_reset_baud();
    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
