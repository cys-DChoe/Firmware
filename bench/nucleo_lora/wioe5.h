/* Seeed Wio-E5 (LoRa-E5) AT 주고받기 — Nucleo 벤치 전용
 * 새 PCB 의 RAK3172(RUI3 AT, Core/Src/rak3172.c)와 명령이 달라서 따로 둠.
 * 근거: Seeed LoRa-E5 AT Command Specification — 명령 "AT+...\r\n", 답은 "+명령: ..." 줄들.
 *   가입:  +JOIN: Start → ... → +JOIN: Network joined → +JOIN: Done   (실패면 +JOIN: Join failed)
 *   송신:  +MSGHEX: Start → ... → +MSGHEX: Done
 * UART 는 Core 의 lora_uart_stm32 (LPUART1 PB6/PB7, 9600) 를 그대로 씀.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rak3172.h"   /* rak_io_t */

typedef enum {
    WIO_OK = 0,
    WIO_E_TIMEOUT = -1,
    WIO_E_FAIL = -2,        /* 모듈이 실패를 알림 */
    WIO_E_NOT_JOINED = -3,
    WIO_E_BUSY = -4,
} wio_err_t;

/* 줄 하나를 받을 때마다 부름 (로그 찍기용, NULL 이면 안 부름) */
typedef void (*wio_line_cb)(const char *line);

void wio_init(const rak_io_t *io, wio_line_cb cb);
/* "AT" → "+AT: OK" */
wio_err_t wio_ping(void);
/* 명령 하나 보내고 그 명령 이름으로 시작하는 줄 하나를 기다림 (AT+ID, AT+DR 같은 짧은 것) */
wio_err_t wio_query(const char *cmd, uint32_t timeout_ms);
/* 명령 하나 보내고 ms 동안 오는 줄을 모두 받음 (여러 줄 답 — AT+CH 등, 줄은 cb 로 나감) */
void wio_dump(const char *cmd, uint32_t ms);
/* 가입 (OTAA, 키 · 대역은 모듈에 저장된 값) — +JOIN: Done 까지 기다림 */
wio_err_t wio_join(uint32_t timeout_ms);
/* 확인 없는 송신 — +MSGHEX: Done 까지 기다림 */
wio_err_t wio_send_hex(const uint8_t *data, size_t len, uint32_t timeout_ms);
