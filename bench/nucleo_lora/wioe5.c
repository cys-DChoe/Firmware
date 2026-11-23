#include <stdio.h>
#include <string.h>
#include "wioe5.h"

#define LINE_MAX 160u

static const rak_io_t *s_io;
static wio_line_cb s_cb;
static char s_line[LINE_MAX];
static size_t s_len;

void wio_init(const rak_io_t *io, wio_line_cb cb)
{
    s_io = io;
    s_cb = cb;
    s_len = 0;
}

/* 받은 글자로 줄을 만듦. 줄이 끝나면 true (s_line 에 \0 로 끝난 줄) */
static bool read_line(uint32_t deadline)
{
    while ((int32_t)(deadline - s_io->ms(s_io->ctx)) > 0) {
        const int c = s_io->getc(s_io->ctx);
        if (c < 0) {
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (s_len == 0u) {
                continue;
            }
            s_line[s_len] = '\0';
            s_len = 0;
            if (s_cb != NULL) {
                s_cb(s_line);
            }
            return true;
        }
        if (s_len < LINE_MAX - 1u) {
            s_line[s_len++] = (char)c;
        }
    }
    return false;
}

static void flush_rx(void)
{
    while (s_io->getc(s_io->ctx) >= 0) {
    }
    s_len = 0;
}

static int send_line(const char *cmd)
{
    flush_rx();
    if (s_io->write(s_io->ctx, cmd, strlen(cmd)) != 0) {
        return -1;
    }
    return s_io->write(s_io->ctx, "\r\n", 2u);
}

wio_err_t wio_ping(void)
{
    if (send_line("AT") != 0) {
        return WIO_E_FAIL;
    }
    const uint32_t end = s_io->ms(s_io->ctx) + 1000u;
    while (read_line(end)) {
        if (strstr(s_line, "+AT: OK") != NULL) {
            return WIO_OK;
        }
    }
    return WIO_E_TIMEOUT;
}

wio_err_t wio_query(const char *cmd, uint32_t timeout_ms)
{
    if (send_line(cmd) != 0) {
        return WIO_E_FAIL;
    }
    /* "AT+ID=DevEui" → 답 머리 "+ID" */
    char head[16] = "+";
    size_t n = 1;
    for (const char *p = cmd + 3; *p != '\0' && *p != '=' && n < sizeof head - 1u; p++) {
        head[n++] = *p;
    }
    head[n] = '\0';
    const uint32_t end = s_io->ms(s_io->ctx) + timeout_ms;
    while (read_line(end)) {
        if (strncmp(s_line, head, n) == 0) {
            return WIO_OK;
        }
    }
    return WIO_E_TIMEOUT;
}

void wio_dump(const char *cmd, uint32_t ms)
{
    if (send_line(cmd) != 0) {
        return;
    }
    const uint32_t end = s_io->ms(s_io->ctx) + ms;
    while (read_line(end)) {
    }
}

static bool has(const char *word) { return strstr(s_line, word) != NULL; }

wio_err_t wio_join(uint32_t timeout_ms)
{
    if (send_line("AT+JOIN") != 0) {
        return WIO_E_FAIL;
    }
    bool joined = false;
    const uint32_t end = s_io->ms(s_io->ctx) + timeout_ms;
    while (read_line(end)) {
        if (strncmp(s_line, "+JOIN", 5) != 0) {
            continue;
        }
        if (has("Joined already")) {
            return WIO_OK;
        }
        if (has("Network joined")) {
            joined = true;
        }
        if (has("busy")) {
            return WIO_E_BUSY;
        }
        if (has("Done")) {      /* 성공이든 실패(+JOIN: Join failed)든 마지막 줄 */
            return joined ? WIO_OK : WIO_E_FAIL;
        }
    }
    return joined ? WIO_OK : WIO_E_TIMEOUT;
}

wio_err_t wio_send_hex(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    static char cmd[16 + 2u * 64u];
    if (len > 64u) {
        return WIO_E_FAIL;
    }
    size_t n = (size_t)snprintf(cmd, sizeof cmd, "AT+MSGHEX=\"");
    for (size_t i = 0; i < len; i++) {
        n += (size_t)snprintf(&cmd[n], sizeof cmd - n, "%02X", data[i]);
    }
    (void)snprintf(&cmd[n], sizeof cmd - n, "\"");
    if (send_line(cmd) != 0) {
        return WIO_E_FAIL;
    }
    wio_err_t res = WIO_E_TIMEOUT;
    const uint32_t end = s_io->ms(s_io->ctx) + timeout_ms;
    while (read_line(end)) {
        if (strncmp(s_line, "+MSGHEX", 7) != 0) {
            continue;
        }
        if (has("join network first")) {
            res = WIO_E_NOT_JOINED;
        } else if (has("busy")) {
            res = WIO_E_BUSY;
        } else if (has("No band") || has("ERROR")) {
            res = WIO_E_FAIL;
        }
        if (has("Done")) {
            return res == WIO_E_TIMEOUT ? WIO_OK : res;
        }
        if (res != WIO_E_TIMEOUT) {
            return res;
        }
    }
    return res;
}
