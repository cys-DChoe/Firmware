#include <stdio.h>
#include <string.h>
#include "fake_lte.h"

void fake_lte_new(fake_lte_t *f, uint32_t *now)
{
    memset(f, 0, sizeof(*f));
    f->now = now;
    f->sim = true;
    f->reg_after = 20;
    f->reg_stat = 1;
    f->qpowd_delay = 3;
}

static uint32_t T(fake_lte_t *f) { return *f->now; }

static void tick(fake_lte_t *f)
{
    if (f->on && f->off_at != 0u && T(f) >= f->off_at) {
        f->on = false;
        f->io = false;
        if (f->key_down) {
            f->key_t = f->off_at;       /* 꺼진 뒤에도 눌려 있으면 그때부터 '켜기 누름'으로 셈 (2 s 넘으면 다시 켜짐) */
        }
        f->off_at = 0;
    }
}

static void say(fake_lte_t *f, const char *s)
{
    for (const char *p = s; *p; p++) {
        const int next = (f->rx_head + 1) % (int)sizeof(f->rx);
        if (next != f->rx_tail) {
            f->rx[f->rx_head] = *p;
            f->rx_head = next;
        }
    }
}

static bool uart_up(fake_lte_t *f) { return f->on && f->io && T(f) >= f->boot_t + 3u; }

static void handle(fake_lte_t *f, const char *c)
{
    f->cmds++;
    char b[96];
    if (strcmp(c, "AT") == 0 || strcmp(c, "ATE0") == 0 || strcmp(c, "AT+CMEE=1") == 0) {
        say(f, "\r\nOK\r\n");
    } else if (strcmp(c, "AT+CPIN?") == 0) {
        say(f, f->sim ? "\r\n+CPIN: READY\r\n\r\nOK\r\n" : "\r\n+CME ERROR: 10\r\n");
    } else if (strcmp(c, "AT+CEREG?") == 0) {
        const unsigned st = (T(f) - f->boot_t >= f->reg_after) ? f->reg_stat : 2u;
        snprintf(b, sizeof(b), "\r\n+CEREG: 0,%u\r\n\r\nOK\r\n", st);
        say(f, b);
    } else if (strcmp(c, "AT+CREG?") == 0) {
        snprintf(b, sizeof(b), "\r\n+CREG: 0,%u\r\n\r\nOK\r\n", (unsigned)f->creg_stat);
        say(f, b);
    } else if (strcmp(c, "AT+CSQ") == 0) {
        say(f, "\r\n+CSQ: 18,99\r\n\r\nOK\r\n");
    } else if (strcmp(c, "AT+COPS?") == 0) {
        say(f, "\r\n+COPS: 0,0,\"SKTelecom\",7\r\n\r\nOK\r\n");
    } else if (strcmp(c, "AT+QPOWD=1") == 0) {
        say(f, "\r\nOK\r\n");
        if (!f->qpowd_ignored) {
            f->off_at = T(f) + f->qpowd_delay;
        }
    } else {
        say(f, "\r\nERROR\r\n");
    }
}

static int w(void *ctx, const char *s, size_t n)
{
    fake_lte_t *f = ctx;
    tick(f);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') {
            continue;
        }
        if (s[i] == '\n') {
            f->tx[f->tx_len] = '\0';
            if (uart_up(f)) {
                handle(f, f->tx);
            }
            f->tx_len = 0;
        } else if (f->tx_len < (int)sizeof(f->tx) - 1) {
            f->tx[f->tx_len++] = s[i];
        }
    }
    return 0;
}

static int g(void *ctx)
{
    fake_lte_t *f = ctx;
    if (f->rx_tail == f->rx_head) {
        return -1;
    }
    const int c = (unsigned char)f->rx[f->rx_tail];
    f->rx_tail = (f->rx_tail + 1) % (int)sizeof(f->rx);
    return c;
}

static uint32_t ms(void *ctx) { return ((fake_lte_t *)ctx)->ms++; }

static void key(void *ctx, bool press)
{
    fake_lte_t *f = ctx;
    tick(f);
    if (press) {
        f->presses++;
        f->key_down = true;
        f->key_t = T(f);
        return;
    }
    if (!f->key_down) {
        return;
    }
    f->key_down = false;
    const uint32_t held = T(f) - f->key_t;
    if (!f->on && !f->dead && held >= 2u) {
        f->on = true;
        f->boot_t = T(f);
        f->off_at = 0;
    } else if (f->on && held >= 3u && !f->key_off_ignored && !f->key_ignored_until_reset) {
        f->off_at = T(f) + 5u;
    }
}

static void rst(void *ctx, bool assert)
{
    fake_lte_t *f = ctx;
    tick(f);
    if (assert) {
        f->rst_down = true;
        f->resets++;
    } else if (f->rst_down) {
        f->rst_down = false;
        if (!f->dead) {                 /* 리셋 = 다시 켜짐 */
            f->on = true;
            f->boot_t = T(f);
            f->off_at = 0;
            f->io = false;
            f->key_ignored_until_reset = false;
        }
    }
}

static bool st(void *ctx)
{
    fake_lte_t *f = ctx;
    tick(f);
    return f->on && T(f) >= f->boot_t + 4u;
}

static bool io(void *ctx, bool on)
{
    fake_lte_t *f = ctx;
    tick(f);
    if (!on) {
        f->io = false;
        return true;
    }
    if (f->no_vddio2 || !f->on || T(f) < f->boot_t + 2u) {
        return false;
    }
    f->io = true;
    return true;
}

void fake_lte_io(fake_lte_t *f, lte_io_t *o)
{
    o->write = w;
    o->getc = g;
    o->ms = ms;
    o->pwrkey = key;
    o->rst = rst;
    o->status = st;
    o->io_on = io;
    o->ctx = f;
}
