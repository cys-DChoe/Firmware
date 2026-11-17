/* LTE 모듈 EG915U — 설명은 lte.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lte.h"

void lte_init(lte_t *l, const lte_io_t *io)
{
    memset(l, 0, sizeof(*l));
    l->io = *io;
}

bool lte_busy(const lte_t *l)
{
    return l->state != LTE_S_IDLE;
}

static void go(lte_t *l, lte_state_t s, uint32_t now)
{
    l->state = s;
    l->t_state = now;
}

static uint32_t since(const lte_t *l, uint32_t now)
{
    return now - l->t_state;
}

/* ---------- 줄 받기 · 명령 ---------- */

static bool read_line(lte_t *l)
{
    int c;
    while ((c = l->io.getc(l->io.ctx)) >= 0) {
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (l->line_len == 0u) {
                continue;
            }
            l->line[l->line_len] = '\0';
            l->line_len = 0;
            return true;
        }
        if (l->line_len < LTE_LINE_MAX - 1u) {
            l->line[l->line_len++] = (char)c;
        }
    }
    return false;
}

/* 명령 하나: OK 면 0, ERROR 면 −2, 시간 넘으면 −1. want 로 시작하는 줄이 오면 그 뒤를 val 에 담음 */
int lte_cmd(lte_t *l, const char *cmd, const char *want, char *val, size_t val_len, uint32_t timeout_ms)
{
    if (val != NULL && val_len > 0u) {
        val[0] = '\0';
    }
    while (read_line(l)) {
        /* 앞서 쌓인 줄(시작 알림 RDY 등)은 버림 */
    }
    l->line_len = 0;
    if (l->io.write(l->io.ctx, cmd, strlen(cmd)) != 0 || l->io.write(l->io.ctx, "\r\n", 2) != 0) {
        return -2;
    }
    const uint32_t t0 = l->io.ms(l->io.ctx);
    for (;;) {
        if (!read_line(l)) {
            if ((uint32_t)(l->io.ms(l->io.ctx) - t0) >= timeout_ms) {
                return -1;
            }
            continue;
        }
        if (strcmp(l->line, cmd) == 0) {
            continue;                           /* 메아리 (ATE0 전) */
        }
        if (strcmp(l->line, "OK") == 0) {
            return 0;
        }
        if (strcmp(l->line, "ERROR") == 0 || strncmp(l->line, "+CME ERROR", 10) == 0) {
            return -2;
        }
        if (want != NULL && val != NULL && strncmp(l->line, want, strlen(want)) == 0) {
            const char *v = l->line + strlen(want);
            while (*v == ' ') {
                v++;
            }
            strncpy(val, v, val_len - 1u);
            val[val_len - 1u] = '\0';
        }
    }
}

/* ---------- 단계 ---------- */

static void begin(lte_t *l, uint32_t now)
{
    memset(&l->res, 0, sizeof(l->res));
    l->res.csq = 99;
    l->res.code = LTE_R_OK;
    l->abort = l->reset_done = l->done = false;
    l->off_way = LTE_OFF_NONE;
    l->t_start = now;
}

bool lte_is_on(const lte_t *l)
{
    return l->io.status != NULL && l->io.status(l->io.ctx);   /* lte_init 전이면 꺼진 것으로 */
}

bool lte_start(lte_t *l, uint32_t now)
{
    if (lte_busy(l)) {
        return false;
    }
    begin(l, now);
    l->tests++;
    if (lte_is_on(l)) {
        l->off_way = LTE_OFF_FAILED;            /* 이미 켜져 있음 — 누르면 꺼지므로 누르지 않음 */
        go(l, LTE_S_IO, now);
        return true;
    }
    l->io.pwrkey(l->io.ctx, true);
    go(l, LTE_S_PRESS, now);
    return true;
}

void lte_stop(lte_t *l)
{
    if (lte_busy(l)) {
        l->abort = true;
    }
}

static void fail(lte_t *l, lte_code_t c)
{
    if (l->res.code == LTE_R_OK) {
        l->res.code = c;
    }
}

static void key_off(lte_t *l, uint32_t now)
{
    l->io.io_on(l->io.ctx, false);              /* 1.8 V 핀을 아날로그로 — 꺼지는 모듈 쪽으로 전압을 안 냄 */
    l->io.pwrkey(l->io.ctx, true);
    go(l, LTE_S_KEY_OFF, now);
}

bool lte_off_start(lte_t *l, uint32_t now)
{
    if (lte_busy(l) || !lte_is_on(l)) {
        return false;
    }
    begin(l, now);
    l->res.code = LTE_R_LEFT_ON;
    l->off_way = LTE_OFF_FAILED;
    key_off(l, now);
    return true;
}

static void finish(lte_t *l, uint32_t now)
{
    l->io.io_on(l->io.ctx, false);
    l->res.off = l->io.status(l->io.ctx) ? LTE_OFF_FAILED : l->off_way;
    l->res.secs = (uint16_t)((now - l->t_start) > 65535u ? 65535u : (now - l->t_start));
    l->done = true;
    go(l, LTE_S_IDLE, now);
}

/* 망에 이미 말이 통하는 단계에서 멈추라면 AT 로 끔 */
static bool talking(lte_state_t s)
{
    return s == LTE_S_AT || s == LTE_S_SIM || s == LTE_S_REG || s == LTE_S_INFO;
}

bool lte_step(lte_t *l, uint32_t now)
{
    if (l->abort && talking(l->state)) {
        fail(l, LTE_R_ABORTED);
        go(l, LTE_S_QPOWD, now);
    }
    char v[48];
    switch (l->state) {
    case LTE_S_IDLE:
        return false;

    case LTE_S_PRESS:
        if (since(l, now) >= LTE_KEY_ON_S) {
            l->io.pwrkey(l->io.ctx, false);
            go(l, LTE_S_WAIT_STATUS, now);
        }
        break;

    case LTE_S_WAIT_STATUS:
        if (l->io.status(l->io.ctx)) {
            l->off_way = LTE_OFF_FAILED;        /* 켜졌음 — 끌 때까지는 '못 끔' */
            if (l->abort) {
                fail(l, LTE_R_ABORTED);
                key_off(l, now);
            } else {
                go(l, LTE_S_IO, now);
            }
        } else if (since(l, now) >= LTE_STATUS_ON_S) {
            fail(l, l->abort ? LTE_R_ABORTED : LTE_R_NO_POWER);
            finish(l, now);
        }
        break;

    case LTE_S_IO:
        if (l->abort) {
            fail(l, LTE_R_ABORTED);
            key_off(l, now);
        } else if (l->io.io_on(l->io.ctx, true)) {
            go(l, LTE_S_AT, now);
        } else if (since(l, now) >= LTE_IO_S) {
            fail(l, LTE_R_NO_IO);
            key_off(l, now);
        }
        break;

    case LTE_S_AT:
        if (lte_cmd(l, "AT", NULL, NULL, 0, 500u) == 0) {
            (void)lte_cmd(l, "ATE0", NULL, NULL, 0, LTE_CMD_MS);
            (void)lte_cmd(l, "AT+CMEE=1", NULL, NULL, 0, LTE_CMD_MS);
            go(l, LTE_S_SIM, now);
        } else if (since(l, now) >= LTE_AT_S) {
            fail(l, LTE_R_NO_AT);
            key_off(l, now);
        }
        break;

    case LTE_S_SIM:
        if (lte_cmd(l, "AT+CPIN?", "+CPIN:", v, sizeof(v), LTE_CMD_MS) == 0 && strstr(v, "READY") != NULL) {
            go(l, LTE_S_REG, now);
        } else if (since(l, now) >= LTE_SIM_S) {
            fail(l, LTE_R_NO_SIM);
            go(l, LTE_S_QPOWD, now);
        }
        break;

    case LTE_S_REG:
        if (lte_cmd(l, "AT+CEREG?", "+CEREG:", v, sizeof(v), LTE_CMD_MS) == 0) {
            const char *comma = strchr(v, ',');   /* "+CEREG: n,stat[,...]" */
            if (comma != NULL) {
                l->res.reg = (uint8_t)atoi(comma + 1);
            }
        }
        if (l->res.reg == 1u || l->res.reg == 5u) {
            go(l, LTE_S_INFO, now);
        } else if (since(l, now) >= LTE_REG_S) {
            fail(l, LTE_R_NO_NET);
            go(l, LTE_S_INFO, now);             /* 그래도 신호 세기는 읽어 둠 */
        }
        break;

    case LTE_S_INFO:
        if (lte_cmd(l, "AT+CSQ", "+CSQ:", v, sizeof(v), LTE_CMD_MS) == 0) {
            l->res.csq = (uint8_t)atoi(v);      /* "rssi,ber" */
        }
        if (lte_cmd(l, "AT+COPS?", "+COPS:", v, sizeof(v), LTE_CMD_MS) == 0) {
            const char *q1 = strchr(v, '"');    /* 0,0,"이름",7 */
            const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 != NULL && q2 != NULL) {
                size_t n = (size_t)(q2 - q1 - 1);
                if (n >= sizeof(l->res.oper)) {
                    n = sizeof(l->res.oper) - 1u;
                }
                memcpy(l->res.oper, q1 + 1, n);
                l->res.oper[n] = '\0';
            }
        }
        go(l, LTE_S_QPOWD, now);
        break;

    case LTE_S_QPOWD:
        (void)lte_cmd(l, "AT+QPOWD=1", NULL, NULL, 0, LTE_CMD_MS);
        l->io.io_on(l->io.ctx, false);
        l->off_way = LTE_OFF_QPOWD;
        go(l, LTE_S_WAIT_OFF, now);
        break;

    case LTE_S_WAIT_OFF:
        if (!l->io.status(l->io.ctx)) {
            finish(l, now);
        } else if (since(l, now) >= LTE_OFF_S) {
            if (l->off_way == LTE_OFF_QPOWD) {
                key_off(l, now);
            } else if (!l->reset_done) {
                l->io.rst(l->io.ctx, true);     /* 100 ms 이상 — 다음 단계(1 s 뒤)에 놓음 */
                go(l, LTE_S_RESET, now);
            } else {
                finish(l, now);                 /* 못 끔 — 결과에 남김 */
            }
        }
        break;

    case LTE_S_KEY_OFF:
        if (!l->io.status(l->io.ctx)) {
            /* 누르는 동안 이미 꺼짐(앞선 AT+QPOWD 가 늦게 먹음 등) → 바로 놓음.
             * 꺼진 모듈에 2 s 넘게 눌려 있으면 다시 켜지기 때문 */
            l->io.pwrkey(l->io.ctx, false);
            if (l->off_way == LTE_OFF_FAILED) {
                l->off_way = LTE_OFF_KEY;
            }
            finish(l, now);
        } else if (since(l, now) >= LTE_KEY_OFF_S) {
            l->io.pwrkey(l->io.ctx, false);
            if (l->off_way != LTE_OFF_RESET) {
                l->off_way = LTE_OFF_KEY;
            }
            go(l, LTE_S_WAIT_OFF, now);
        }
        break;

    case LTE_S_RESET:
        if (since(l, now) >= 1u) {
            l->io.rst(l->io.ctx, false);
            l->reset_done = true;
            l->off_way = LTE_OFF_RESET;
            /* 리셋하면 모듈이 다시 켜짐 → 켜질 시간을 두고 PWRKEY 로 한 번 더 끔 */
            if (since(l, now) >= 1u + LTE_STATUS_ON_S || l->io.status(l->io.ctx)) {
                key_off(l, now);
            }
        }
        break;
    }
    return lte_busy(l);
}

size_t lte_result_encode(const lte_result_t *r, uint8_t out[7])
{
    out[0] = 1u;
    out[1] = (uint8_t)r->code;
    out[2] = (uint8_t)r->off;
    out[3] = r->reg;
    out[4] = r->csq;
    out[5] = (uint8_t)(r->secs >> 8);
    out[6] = (uint8_t)r->secs;
    return 7u;
}

const char *lte_code_name(lte_code_t c)
{
    switch (c) {
    case LTE_R_OK:       return "망 등록됨";
    case LTE_R_NO_POWER: return "안 켜짐";
    case LTE_R_NO_IO:    return "1.8 V 안 올라옴";
    case LTE_R_NO_AT:    return "AT 대답 없음";
    case LTE_R_NO_SIM:   return "유심 없음";
    case LTE_R_NO_NET:   return "망 등록 안 됨";
    case LTE_R_ABORTED:  return "중간에 멈춤";
    case LTE_R_LEFT_ON:  return "켜져 있어서 끔";
    default:             return "모름";
    }
}
