#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_rak.h"

/* ---------- 가짜 모듈 ---------- */


void push(fake_t *f, const char *s)
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

char *val_of(fake_t *f, const char *key)
{
    for (int i = 0; i < f->nkeys; i++) {
        if (strcmp(f->keys[i], key) == 0) {
            return f->vals[i];
        }
    }
    return NULL;
}

void set_key(fake_t *f, const char *key, const char *v)
{
    char *p = val_of(f, key);
    if (p == NULL) {
        f->keys[f->nkeys] = key;
        p = f->vals[f->nkeys++];
    }
    strncpy(p, v, 79);
}

void status(fake_t *f, const char *st) { pushf(f, "\r\n", st); push(f, "\r\n"); }

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
                              "AT+ADR", "AT+LBT", "AT+LBTRSSI", "AT+LBTSCANTIME", "AT+LPMLVL", "AT+LPM", "AT+NJS", "AT+TIMEREQ"};
    static const char *V[] = {"1", "1", "4", "AC1F09FFFE000000", "0000000000000000",
                              "00000000000000000000000000000000", "0", "1", "0", "-80", "5", "1", "0", "0", "0"};
    for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++) {
        set_key(f, K[i], V[i]);
    }
}

static void handle(fake_t *f, const char *cmd)
{
    if (f->dead || f->mod_baud != f->mcu_baud) {
        return;                 /* 속도가 다르면 깨진 글자 → 답 없음 */
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
    } else if (strcmp(cmd, "AT+LTIME=?") == 0) {
        /* 매뉴얼 예 그대로: 값 줄 앞에 "AT+" 가 없음 */
        pushf(f, "\r\nLTIME: ", f->ltime[0] ? f->ltime : "00h00m00s on 01/01/2000");
        push(f, "\r\n");
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
        f->saved_baud = (uint32_t)strtoul(cmd + 8, NULL, 10);
        if (!f->baud_after_reset) {
            f->mod_baud = f->saved_baud;
        }
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

int fk_write(void *ctx, const char *s, size_t n)
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

int fk_getc(void *ctx)
{
    fake_t *f = ctx;
    if (f->head == f->tail) {
        f->head = f->tail = 0;      /* 다 읽었으면 처음부터 다시 씀 (긴 시험에서 줄 버퍼가 넘치지 않게) */
        f->clock++;                 /* 받을 게 없으면 1 ms 흐름 */
        return -1;
    }
    return (unsigned char)f->rxq[f->head++];
}

uint32_t fk_ms(void *ctx) { return ((fake_t *)ctx)->clock; }
void fk_delay(void *ctx, uint32_t ms) { ((fake_t *)ctx)->clock += ms; }

void fk_rst(void *ctx, bool level)
{
    fake_t *f = ctx;
    if (!level) {
        f->rst_low_ms = f->clock;
    } else if (!f->rst_level) {
        f->rst_low_ms = f->clock - f->rst_low_ms;
        f->resets++;
        f->asleep = false;
        f->mod_baud = f->saved_baud;
        if (f->mod_baud == f->mcu_baud) {
            banner(f);
        }
    }
    f->rst_level = level;
}

void fk_set_baud(void *ctx, uint32_t baud)
{
    fake_t *f = ctx;
    f->mcu_baud = baud;
    f->baud_switches++;
}

fake_t F;
rak_t R;

void fresh(void)
{
    fake_factory(&F);
    F.mod_baud = F.mcu_baud = F.saved_baud = 115200;
    rak_io_t io = {fk_write, fk_getc, fk_ms, fk_delay, fk_rst, &F, fk_set_baud};
    rak_init(&R, &io);
}

int count_writes(const char *prefix)
{
    int n = 0;
    for (int i = 0; i < F.nlog; i++) {
        if (strncmp(F.log[i], prefix, strlen(prefix)) == 0 && strstr(F.log[i], "=?") == NULL) {
            n++;
        }
    }
    return n;
}

