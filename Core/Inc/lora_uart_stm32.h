/* 보드 쪽 LoRa 모듈 연결 — LPUART1 (PB6 TX → RAK3172 UART2_RX, PB7 RX ← UART2_TX, AF8) · PE2 → NRST
 * 받기는 인터럽트로 고리 버퍼에 쌓고, rak3172.c 는 getc 로 꺼내 씀. */
#pragma once

#include "rak3172.h"
#include "stm32h5xx_hal.h"

#define LORA_RX_BUF  1024u   /* 2의 거듭제곱 */

typedef struct {
    UART_HandleTypeDef h;
    volatile uint16_t head, tail;
    volatile uint32_t overruns;
    uint8_t buf[LORA_RX_BUF];
    rak_io_t io;
} lora_uart_t;

/* baud: 처음엔 모듈 기본 115200. 모듈에 AT+BAUD=9600 저장한 뒤엔 9600 (잠 중에도 LSE 로 받기, 6번 작업). */
HAL_StatusTypeDef lora_uart_init(lora_uart_t *u, uint32_t baud);
/* stm32h5xx_it.c 의 LPUART1_IRQHandler 에서 부름 */
void lora_uart_irq(lora_uart_t *u);
extern lora_uart_t g_lora_uart;
