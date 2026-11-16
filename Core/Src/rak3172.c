#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lowpower.h"
#include "rak3172.h"

#define WAKE_TRIES        3
#define WAKE_TIMEOUT_MS   200u

void rak_init(rak_t *r, const rak_io_t *io)
{
    memset(r, 0, sizeof(*r));
    r->io = *io;
    r->last_tx = RAK_EV_NONE;
}

void rak_default_cfg(rak_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->band = 7;             /* 국내 시험 KR920. 케냐 현장은 4 (EU868) — 국내에서 868 MHz 쏘면 안 됨 */
    c->confirmed = false;
    c->adr = true;
    c->lbt = true;           /* KR920 은 LBT 필요 */
    c->lbt_rssi = -65;       /* 고시 문턱 −65 dBm */
    c->lbt_scan = 5;         /* 고시 5 ms 이상 — 값의 단위는 벤치에서 확인 */
    c->lpm = true;
    c->lpm_level = 2;        /* STOP2: UART1 로는 못 깨지만 우리가 쓰는 UART2 로는 깸 */
}

/* ---------- 문자열 도우미 ---------- */

size_t rak_hex_encode(const uint8_t *in, size_t n, char *out, size_t out_len)
{
    static const char H[] = "0123456789ABCDEF";
    if (out_len < 2u * n + 1u) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 0x0F];
    }
    out[2 * n] = '\0';
    return 2 * n;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int rak_hex_decode(const char *in, uint8_t *out, size_t out_max)
{
    size_t n = strlen(in);
    if (n % 2u != 0u || n / 2u > out_max) {
        return -1;
    }
    for (size_t i = 0; i < n / 2u; i++) {
        int hi = hexval(in[2 * i]), lo = hexval(in[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2u);
}

/* 상태 줄인지 보고, 맞으면 결과를 돌려줌. 매뉴얼 목록: OK · AT_ERROR · AT_PARAM_ERROR · AT_BUSY_ERROR ·
 * AT_TEST_PARAM_OVERFLOW · AT_NO_CLASSB_ENABLE · AT_NO_NETWORK_JOINED · AT_RX_ERROR (+ 예제의 COMMAND_LOCKED) */
rak_err_t rak_parse_status(const char *line, bool *is_status)
{
    *is_status = true;
    if (strcmp(line, "OK") == 0) return RAK_OK;
    if (strcmp(line, "AT_PARAM_ERROR") == 0) return RAK_E_PARAM;
    if (strcmp(line, "AT_BUSY_ERROR") == 0) return RAK_E_BUSY;
    if (strcmp(line, "AT_NO_NETWORK_JOINED") == 0) return RAK_E_NOT_JOINED;
    if (strncmp(line, "AT_", 3) == 0 || strcmp(line, "COMMAND_LOCKED") == 0) return RAK_E_AT;
    *is_status = false;
    return RAK_OK;
}

/* "+EVT:..." 줄 풀기. 매뉴얼 안에서도 "+EVT:JOIN FAILED" · "+EVT: SEND CONFIRMED OK" 처럼
 * 띄어쓰기가 섞여 있어서, 이름 부분은 앞 공백을 빼고 공백을 '_' 로 바꿔 비교함. */
bool rak_parse_event(const char *line, rak_event_t *ev)
{
    memset(ev, 0, sizeof(*ev));
    const char *p = line;
    if (strncmp(p, "+EVT:", 5) == 0) {
        p += 5;
        while (*p == ' ') p++;
    } else if (strncmp(p, "RX_", 3) != 0) {
        /* 매뉴얼 표의 수신 예는 앞머리 없이 "RX_1:..." 로도 적혀 있어 둘 다 받음 */
        if (strcmp(p, "AT_NO_NETWORK_JOINED") == 0) {
            ev->kind = RAK_EV_NOT_JOINED;
            return true;
        }
        return false;
    }

    char name[40];
    size_t i = 0;
    for (; p[i] && p[i] != ':' && i < sizeof(name) - 1; i++) {
        name[i] = (p[i] == ' ') ? '_' : (char)toupper((unsigned char)p[i]);
    }
    name[i] = '\0';
    const char *rest = (p[i] == ':') ? p + i + 1 : NULL;

    if (strcmp(name, "JOINED") == 0) {
        ev->kind = RAK_EV_JOINED;
    } else if (strncmp(name, "JOIN_FAILED", 11) == 0) {
        ev->kind = RAK_EV_JOIN_FAILED;
    } else if (strcmp(name, "TX_DONE") == 0) {
        ev->kind = RAK_EV_TX_DONE;
    } else if (strcmp(name, "SEND_CONFIRMED_OK") == 0) {
        ev->kind = RAK_EV_CONFIRMED_OK;
    } else if (strcmp(name, "SEND_CONFIRMED_FAILED") == 0) {
        ev->kind = RAK_EV_CONFIRMED_FAILED;
    } else if (strncmp(name, "RX_", 3) == 0 && rest != NULL) {
        /* RX_1:<rssi>:<snr>:UNICAST:<port>:<16진> — 예 "RX_1:-70:8:UNICAST:1:1234" */
        char buf[RAK_LINE_MAX];
        strncpy(buf, rest, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *f[5] = {0};
        int nf = 0;
        char *s = buf;
        f[nf++] = s;
        while (nf < 5 && (s = strchr(s, ':')) != NULL) {
            *s++ = '\0';
            f[nf++] = s;
        }
        if (nf < 4) {
            return false;
        }
        ev->kind = RAK_EV_RX;
        ev->rssi = (int16_t)strtol(f[0], NULL, 10);
        ev->snr = (int8_t)strtol(f[1], NULL, 10);
        ev->port = (uint8_t)strtoul(f[3], NULL, 10);
        if (nf == 5 && f[4][0] != '\0') {
            int n = rak_hex_decode(f[4], ev->data, sizeof(ev->data));
            if (n < 0) {
                return false;
            }
            ev->len = (uint8_t)n;
        }
    } else {
        ev->kind = RAK_EV_OTHER;
    }
    return true;
}

/* ---------- 줄 받기 ---------- */

static void apply_event(rak_t *r, const rak_event_t *ev)
{
    switch (ev->kind) {
    case RAK_EV_JOINED:
        r->joined = true;
        r->joining = false;
        break;
    case RAK_EV_JOIN_FAILED:
        r->joining = false;
        r->join_fail_count++;
        break;
    case RAK_EV_TX_DONE:
        /* 확인 송신이면 결과(CONFIRMED_*)까지 기다림 */
        if (!r->confirmed) {
            r->tx_pending = false;
            r->last_tx = RAK_EV_TX_DONE;
        }
        break;
    case RAK_EV_CONFIRMED_OK:
    case RAK_EV_CONFIRMED_FAILED:
        r->tx_pending = false;
        r->last_tx = ev->kind;
        break;
    case RAK_EV_NOT_JOINED:
        r->tx_pending = false;
        r->joined = false;
        r->last_tx = RAK_EV_NOT_JOINED;
        break;
    case RAK_EV_RX:
        r->rx = *ev;
        r->rx_new = true;
        break;
    default:
        break;
    }
}

/* 받은 글자로 줄 하나가 완성되면 true. '\r' 는 버리고 빈 줄은 건너뜀. */
static bool feed(rak_t *r, int c)
{
    if (c == '\r') {
        return false;
    }
    if (c == '\n') {
        bool done = r->line_len > 0 && !r->line_overflow;
        r->line[r->line_len] = '\0';
        if (!done) {
            r->line_len = 0;
        }
        r->line_overflow = false;
        return done;
    }
    if (r->line_len < RAK_LINE_MAX - 1u) {
        r->line[r->line_len++] = (char)c;
    } else {
        r->line_overflow = true;     /* 너무 긴 줄은 통째로 버림 */
    }
    return false;
}

static void take_line(rak_t *r, char *out, size_t n)
{
    strncpy(out, r->line, n - 1);
    out[n - 1] = '\0';
    r->line_len = 0;
}

rak_ev_kind_t rak_poll(rak_t *r)
{
    rak_ev_kind_t last = RAK_EV_NONE;
    int c;
    while ((c = r->io.getc(r->io.ctx)) >= 0) {
        if (feed(r, c)) {
            char line[RAK_LINE_MAX];
            take_line(r, line, sizeof(line));
            rak_event_t ev;
            if (rak_parse_event(line, &ev)) {
                apply_event(r, &ev);
                last = ev.kind;
            }
        }
    }
    return last;
}

/* ---------- 명령 ---------- */

static rak_err_t cmd_raw(rak_t *r, const char *cmd, char *val, size_t val_len, uint32_t timeout_ms)
{
    if (val != NULL && val_len > 0) {
        val[0] = '\0';
    }
    /* 명령 보내기 전에 쌓인 이벤트부터 처리 */
    rak_poll(r);

    const size_t n = strlen(cmd);
    if (r->io.write(r->io.ctx, cmd, n) != 0 || r->io.write(r->io.ctx, "\r\n", 2) != 0) {
        return RAK_E_IO;
    }

    /* 값 줄 앞머리: "AT+NJS=?" → "AT+NJS=" */
    const char *eq = strchr(cmd, '=');
    const size_t key_len = eq ? (size_t)(eq - cmd) + 1u : 0u;

    const uint32_t t0 = r->io.ms(r->io.ctx);
    for (;;) {
        int c = r->io.getc(r->io.ctx);
        if (c < 0) {
            if ((uint32_t)(r->io.ms(r->io.ctx) - t0) >= timeout_ms) {
                return RAK_E_TIMEOUT;
            }
            continue;
        }
        if (!feed(r, c)) {
            continue;
        }
        char line[RAK_LINE_MAX];
        take_line(r, line, sizeof(line));

        if (strcmp(line, cmd) == 0) {
            continue;                                /* 메아리(ATE 켜짐) */
        }
        bool is_status;
        const rak_err_t st = rak_parse_status(line, &is_status);
        if (is_status) {
            return st;
        }
        rak_event_t ev;
        if (strncmp(line, "+EVT", 4) == 0 && rak_parse_event(line, &ev)) {
            apply_event(r, &ev);                     /* 명령 중 끼어든 이벤트 */
            continue;
        }
        if (key_len > 0 && val != NULL && strncmp(line, cmd, key_len) == 0) {
            strncpy(val, line + key_len, val_len - 1);
            val[val_len - 1] = '\0';
        } else if (key_len > 4 && val != NULL && val[0] == '\0' && strncmp(line, cmd + 3, key_len - 4) == 0 &&
                   line[key_len - 4] == ':') {
            /* 몇몇 질의는 "AT+" 없이 "LTIME: 04h36m00s on 11/27/2023" 처럼 답함 */
            const char *v = line + key_len - 3;
            while (*v == ' ') {
                v++;
            }
            strncpy(val, v, val_len - 1);
            val[val_len - 1] = '\0';
        }
        /* 그 밖의 줄(시작 문구 · 도움말)은 버림 */
    }
}

rak_err_t rak_wake(rak_t *r)
{
    rak_err_t e = RAK_E_TIMEOUT;
    for (int i = 0; i < WAKE_TRIES; i++) {
        e = cmd_raw(r, "AT", NULL, 0, WAKE_TIMEOUT_MS);
        if (e == RAK_OK) {
            break;
        }
    }
    return e;
}

rak_err_t rak_cmd(rak_t *r, const char *cmd, char *val, size_t val_len)
{
    if (r->lpm) {
        /* 잠든 모듈은 깨우는 첫 글자를 잃을 수 있어 "AT" 로 먼저 깨움 [가정 — 보드에서 확인] */
        const rak_err_t w = rak_wake(r);
        if (w != RAK_OK) {
            return w;
        }
    }
    const rak_err_t e = cmd_raw(r, cmd, val, val_len, RAK_CMD_TIMEOUT_MS);
    if (e == RAK_OK && strchr(cmd, '=') != NULL && strstr(cmd, "=?") == NULL) {
        r->writes++;
    }
    return e;
}

rak_err_t rak_hw_reset(rak_t *r)
{
    r->io.rst(r->io.ctx, false);
    r->io.delay_ms(r->io.ctx, RAK_RESET_PULSE_MS);
    r->io.rst(r->io.ctx, true);
    r->io.delay_ms(r->io.ctx, RAK_BOOT_MS);
    rak_poll(r);                      /* 시작 문구 버리기 */
    r->joined = r->joining = r->tx_pending = false;
    return rak_wake(r);
}

/* 지금 값을 읽어 다르면 씀. 같으면 쓰지 않음(모듈 플래시 아끼기 · 재시작 피하기). */
static rak_err_t ensure(rak_t *r, const char *key, const char *want)
{
    char q[48], v[80];
    snprintf(q, sizeof(q), "%s=?", key);
    rak_err_t e = rak_cmd(r, q, v, sizeof(v));
    if (e != RAK_OK) {
        return e;
    }
    /* 16진 키는 대소문자 구분 없이 비교 */
    bool same = strlen(v) == strlen(want);
    for (size_t i = 0; same && want[i]; i++) {
        same = toupper((unsigned char)v[i]) == toupper((unsigned char)want[i]);
    }
    if (same) {
        return RAK_OK;
    }
    char s[RAK_LINE_MAX];
    snprintf(s, sizeof(s), "%s=%s", key, want);
    return rak_cmd(r, s, NULL, 0);
}

static bool hex_ok(const char *s, size_t n)
{
    if (s == NULL || strlen(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (hexval(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

rak_err_t rak_configure(rak_t *r, const rak_cfg_t *c)
{
    if (!hex_ok(c->dev_eui, 16) || !hex_ok(c->app_eui, 16) || !hex_ok(c->app_key, 32)) {
        return RAK_E_ARG;
    }
    char v[16];
    /* 1) LoRaWAN 모드 — 바꾸면 모듈이 다시 켜짐 */
    rak_err_t e = rak_cmd(r, "AT+NWM=?", v, sizeof(v));
    if (e != RAK_OK) {
        return e;
    }
    if (strcmp(v, "1") != 0) {
        e = rak_cmd(r, "AT+NWM=1", NULL, 0);
        if (e != RAK_OK) {
            return e;
        }
        r->io.delay_ms(r->io.ctx, RAK_BOOT_MS);
        rak_poll(r);
        e = rak_wake(r);
        if (e != RAK_OK) {
            return e;
        }
    }

    typedef struct { const char *key; const char *val; } item_t;
    item_t items[12];
    int n = 0;
    char band[4], cfm[2], adr[2], lbt[2], rssi[8], scan[8], lpm[2], lvl[4];
    snprintf(band, sizeof(band), "%u", c->band);
    snprintf(cfm, sizeof(cfm), "%d", c->confirmed ? 1 : 0);
    snprintf(adr, sizeof(adr), "%d", c->adr ? 1 : 0);
    snprintf(lbt, sizeof(lbt), "%d", c->lbt ? 1 : 0);
    snprintf(rssi, sizeof(rssi), "%d", c->lbt_rssi);
    snprintf(scan, sizeof(scan), "%u", c->lbt_scan);
    snprintf(lpm, sizeof(lpm), "%d", c->lpm ? 1 : 0);
    snprintf(lvl, sizeof(lvl), "%u", c->lpm_level);

    items[n++] = (item_t){"AT+NJM", "1"};          /* OTAA */
    items[n++] = (item_t){"AT+BAND", band};
    items[n++] = (item_t){"AT+DEVEUI", c->dev_eui};
    items[n++] = (item_t){"AT+APPEUI", c->app_eui};
    items[n++] = (item_t){"AT+APPKEY", c->app_key};
    items[n++] = (item_t){"AT+CFM", cfm};
    items[n++] = (item_t){"AT+ADR", adr};
    if (c->lbt) {
        items[n++] = (item_t){"AT+LBT", lbt};
        items[n++] = (item_t){"AT+LBTRSSI", rssi};
        items[n++] = (item_t){"AT+LBTSCANTIME", scan};
    }
    if (c->lpm) {
        items[n++] = (item_t){"AT+LPMLVL", lvl};
    }
    items[n++] = (item_t){"AT+LPM", lpm};          /* 맨 끝: 켜는 순간부터 깨우기가 필요 */

    for (int i = 0; i < n; i++) {
        e = ensure(r, items[i].key, items[i].val);
        if (e != RAK_OK) {
            return e;
        }
        if (strcmp(items[i].key, "AT+LPM") == 0) {
            r->lpm = c->lpm;
        }
    }
    r->confirmed = c->confirmed;
    return RAK_OK;
}

rak_err_t rak_join_start(rak_t *r)
{
    /* 가입 · 자동가입 끔 · 10 s 간격 · 8번 — 결과는 +EVT:JOINED / JOIN_FAILED_… */
    const rak_err_t e = rak_cmd(r, "AT+JOIN=1:0:10:8", NULL, 0);
    if (e == RAK_OK) {
        r->joining = true;
        r->joined = false;
    }
    return e;
}

rak_err_t rak_send_start(rak_t *r, uint8_t port, const uint8_t *data, size_t len)
{
    if (port < 1u || port > 233u || len == 0u || len > RAK_PAYLOAD_MAX) {
        return RAK_E_ARG;
    }
    if (r->tx_pending) {
        return RAK_E_BUSY;
    }
    char s[RAK_LINE_MAX];
    int k = snprintf(s, sizeof(s), "AT+SEND=%u:", port);
    rak_hex_encode(data, len, s + k, sizeof(s) - (size_t)k);
    const rak_err_t e = rak_cmd(r, s, NULL, 0);
    if (e == RAK_OK) {
        r->tx_pending = true;
        r->last_tx = RAK_EV_NONE;
        r->tx_count++;
    } else if (e == RAK_E_NOT_JOINED) {
        r->joined = false;
    }
    return e;
}

rak_err_t rak_set_baud(rak_t *r, uint32_t baud)
{
    char s[32];
    snprintf(s, sizeof(s), "AT+BAUD=%lu", (unsigned long)baud);
    return rak_cmd(r, s, NULL, 0);
}

rak_err_t rak_ensure_baud(rak_t *r, uint32_t want, uint32_t old, bool *was_set)
{
    if (was_set != NULL) {
        *was_set = false;
    }
    if (r->io.set_baud == NULL) {
        return RAK_E_ARG;
    }
    r->io.set_baud(r->io.ctx, want);
    if (rak_wake(r) == RAK_OK) {
        return RAK_OK;                       /* 이미 맞춰져 있음 */
    }
    r->io.set_baud(r->io.ctx, old);
    rak_err_t e = rak_wake(r);
    if (e != RAK_OK) {
        return e;                            /* 두 속도 다 응답 없음 */
    }
    e = rak_set_baud(r, want);
    if (e != RAK_OK) {
        return e;
    }
    if (was_set != NULL) {
        *was_set = true;
    }
    r->io.set_baud(r->io.ctx, want);
    if (rak_wake(r) == RAK_OK) {
        return RAK_OK;
    }
    /* 저장만 되고 다시 켜야 바뀌는 경우 [매뉴얼에 없음 — 보드에서 확인] */
    return rak_hw_reset(r);
}

/* "04h36m00s on 11/27/2023" (매뉴얼 예, UTC+0) → Unix 초. 날짜는 예처럼 월/일/연으로 읽고,
 * 앞 수가 12 를 넘으면 일/월/연으로 봄. 연도가 2026–2099 가 아니면(아직 못 맞춘 시각) false. */
bool rak_parse_ltime(const char *s, uint32_t *unix_s)
{
    unsigned h, m, sec, a, b, y;
    if (sscanf(s, "%uh%um%us on %u/%u/%u", &h, &m, &sec, &a, &b, &y) != 6) {
        return false;
    }
    unsigned mon = a, day = b;
    if (a > 12u) {
        mon = b;
        day = a;
    }
    if (y < 2026u || y > 2099u || mon < 1u || mon > 12u || day < 1u || day > 31u || h > 23u || m > 59u || sec > 59u) {
        return false;
    }
    const lp_civil_t c = {(uint16_t)y, (uint8_t)mon, (uint8_t)day, (uint8_t)h, (uint8_t)m, (uint8_t)sec};
    *unix_s = lp_unix_from_civil(&c);
    return true;
}

rak_err_t rak_time_request(rak_t *r, bool on)
{
    return rak_cmd(r, on ? "AT+TIMEREQ=1" : "AT+TIMEREQ=0", NULL, 0);
}

rak_err_t rak_get_time(rak_t *r, uint32_t *unix_s)
{
    char v[48];
    const rak_err_t e = rak_cmd(r, "AT+LTIME=?", v, sizeof(v));
    if (e != RAK_OK) {
        return e;
    }
    return rak_parse_ltime(v, unix_s) ? RAK_OK : RAK_E_PARAM;
}
