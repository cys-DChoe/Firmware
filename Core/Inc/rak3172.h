/* LoRa 모듈 RAK3172 (RUI3 AT) 주고받기
 *
 * 근거: RUI3 AT Command Manual (docs.rakwireless.com, 11/14 · 11/16 읽음) · Docs/GasMeter_19_펌웨어_주의.html 8번
 *   - 명령은 "AT+...\r\n", 답은 값 줄("AT+NJS=1") 다음 상태 줄("OK" 또는 "AT_..._ERROR")
 *   - 오래 걸리는 일(가입 · 송신)은 OK 가 먼저 오고 결과는 나중에 "+EVT:..." 줄로 옴
 *
 * 그래서 이 모듈은 두 가지로 나눔:
 *   rak_cmd()      명령 하나 보내고 상태 줄까지 기다림(짧음, 1 s 이내)
 *   rak_poll()     주 루프에서 자주 불러 들어온 +EVT 줄을 처리(기다리지 않음)
 * 가입 · 송신은 *_start() 로 시작만 하고, 결과는 rak_t 의 joined · tx_pending · rx 로 봄.
 * 그동안 주 루프는 1초 측정을 계속할 수 있음.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAK_LINE_MAX     540u    /* AT+SEND=233: + 16진 484자 + 여유 */
#define RAK_PAYLOAD_MAX  242u    /* 이 펌웨어가 한 번에 보내는 최대 바이트 (매뉴얼 한도 250) */
#define RAK_RX_MAX       64u     /* 내려오는 값 최대 바이트 (설정 바꾸기용, 넉넉히) */
#define RAK_CMD_TIMEOUT_MS   1000u
#define RAK_BOOT_MS          1500u   /* 리셋 · 모드 바꾼 뒤 기다림 [가정 — 보드에서 잼] */
#define RAK_RESET_PULSE_MS   2u      /* NRST 낮음 유지 [가정 — STM32 NRST 일반값 1 ms 이상] */

typedef enum {
    RAK_OK = 0,
    RAK_E_TIMEOUT = -1,      /* 상태 줄이 안 옴 */
    RAK_E_AT = -2,           /* AT_ERROR 와 그 밖의 오류 줄 */
    RAK_E_PARAM = -3,        /* AT_PARAM_ERROR */
    RAK_E_BUSY = -4,         /* AT_BUSY_ERROR — 앞 송신 중 · 듀티 대기 · 수신 창 */
    RAK_E_NOT_JOINED = -5,   /* AT_NO_NETWORK_JOINED */
    RAK_E_ARG = -6,          /* 이쪽 인자 잘못 (너무 김 등) */
    RAK_E_IO = -7,           /* UART 쓰기 실패 */
} rak_err_t;

/* 보드 쪽 연결. getc 는 받은 글자가 없으면 -1 (기다리지 않음). */
typedef struct {
    int (*write)(void *ctx, const char *s, size_t n);
    int (*getc)(void *ctx);
    uint32_t (*ms)(void *ctx);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void (*rst)(void *ctx, bool level);     /* false = 리셋 걸기(낮음), true = 풀기 */
    void *ctx;
    void (*set_baud)(void *ctx, uint32_t baud);   /* MCU 쪽 UART 속도 바꾸기 (없으면 NULL) */
} rak_io_t;

typedef enum {
    RAK_EV_NONE = 0,
    RAK_EV_JOINED,
    RAK_EV_JOIN_FAILED,
    RAK_EV_TX_DONE,
    RAK_EV_CONFIRMED_OK,
    RAK_EV_CONFIRMED_FAILED,
    RAK_EV_RX,
    RAK_EV_NOT_JOINED,       /* 송신 뒤 늦게 온 AT_NO_NETWORK_JOINED */
    RAK_EV_OTHER,            /* 알아본 +EVT 이지만 쓰지 않는 것 */
} rak_ev_kind_t;

typedef struct {
    rak_ev_kind_t kind;
    int16_t rssi;            /* RX 만 */
    int8_t snr;
    uint8_t port;
    uint8_t len;
    uint8_t data[RAK_RX_MAX];
} rak_event_t;

typedef struct {
    uint8_t band;            /* 7 = KR920 (국내 시험) · 4 = EU868 (케냐) */
    const char *dev_eui;     /* 16진 16자 */
    const char *app_eui;     /* 16진 16자 */
    const char *app_key;     /* 16진 32자 — secrets.h 에서, 저장소에 안 올림 */
    bool confirmed;          /* false: 확인 없음 (1분 보고는 확인 없이) */
    bool adr;
    bool lbt;                /* KR920 이면 켬 (고시: −65 dBm · 5 ms 이상) */
    int16_t lbt_rssi;        /* dBm */
    uint16_t lbt_scan;       /* AT+LBTSCANTIME 값 — 단위는 매뉴얼에 없음, 벤치에서 확인 */
    bool lpm;                /* 명령 뒤 모듈 스스로 잠 */
    uint8_t lpm_level;       /* 1 = STOP1, 2 = STOP2 (UART2 로는 둘 다 깸) */
} rak_cfg_t;

typedef struct {
    rak_io_t io;
    bool lpm;                /* 켜져 있으면 명령 전에 "AT" 로 깨움 */
    bool confirmed;
    bool joining, joined;
    bool tx_pending;
    rak_ev_kind_t last_tx;   /* TX_DONE · CONFIRMED_* · NOT_JOINED */
    bool rx_new;
    rak_event_t rx;          /* 마지막으로 내려온 값 */
    uint32_t join_fail_count, tx_count, writes;
    /* 줄 모으기 */
    char line[RAK_LINE_MAX];
    size_t line_len;
    bool line_overflow;
} rak_t;

void rak_init(rak_t *r, const rak_io_t *io);
void rak_default_cfg(rak_cfg_t *c);

/* 명령 하나: cmd 는 "\r\n" 없이. 값 줄이 있으면 '=' 뒤를 val 에 담음(없으면 빈 문자열). */
rak_err_t rak_cmd(rak_t *r, const char *cmd, char *val, size_t val_len);
/* 들어온 글자를 처리. 이번에 처리한 마지막 이벤트 종류를 돌려줌(없으면 RAK_EV_NONE). */
rak_ev_kind_t rak_poll(rak_t *r);

rak_err_t rak_hw_reset(rak_t *r);                 /* NRST 펄스 → 기다림 → "AT" 응답 확인 */
rak_err_t rak_wake(rak_t *r);                     /* "AT" 를 최대 3번 — 잠에서 깨우기 */
rak_err_t rak_configure(rak_t *r, const rak_cfg_t *c);   /* 다른 값만 씀 (모듈 플래시 아끼기) */
rak_err_t rak_join_start(rak_t *r);              /* AT+JOIN=1:0:10:8 */
rak_err_t rak_send_start(rak_t *r, uint8_t port, const uint8_t *data, size_t len);
rak_err_t rak_set_baud(rak_t *r, uint32_t baud);  /* 모듈에 저장됨 — 부른 뒤 MCU 쪽 속도도 바꿀 것 */
/* 모듈과 want 속도로 말이 통하게 맞춤 (잠 중에도 받으려면 9600, 설계서 결정 ⑦):
 *   want 로 "AT" → 되면 끝. 안 되면 old(공장값 115200)로 "AT" → AT+BAUD=want → MCU 도 want →
 *   바로 안 통하면 리셋 뒤 다시 확인. *was_set 에 이번에 모듈 설정을 바꿨는지. */
rak_err_t rak_ensure_baud(rak_t *r, uint32_t want, uint32_t old, bool *was_set);

/* 아래는 맥 시험용으로 드러내 둠 */
rak_err_t rak_parse_status(const char *line, bool *is_status);
bool rak_parse_event(const char *line, rak_event_t *ev);
size_t rak_hex_encode(const uint8_t *in, size_t n, char *out, size_t out_len);
int rak_hex_decode(const char *in, uint8_t *out, size_t out_max);
