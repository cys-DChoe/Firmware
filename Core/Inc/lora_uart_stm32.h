/* 보드 쪽 LoRa 모듈 연결 — LPUART1 (PB6 TX → RAK3172 UART2_RX, PB7 RX ← UART2_TX, AF8) · PE2 → NRST
 * 받기는 인터럽트로 고리 버퍼에 쌓고, rak3172.c 는 getc 로 꺼내 씀. */
#pragma once

#include "rak3172.h"
#include "stm32h5xx_hal.h"

#define LORA_RX_BUF  1024u   /* 2의 거듭제곱 */
#define LORA_LSE_MAX_BAUD  9600u   /* LSE 32.768 kHz 로 낼 수 있는 가장 빠른 속도 (lowpower.h) */

typedef struct {
    UART_HandleTypeDef h;
    volatile uint16_t head, tail;
    volatile uint32_t overruns;
    uint8_t buf[LORA_RX_BUF];
    rak_io_t io;
} lora_uart_t;

/* baud: 9600 이하면 LSE 로 돌려 Stop 중에도 받고 깨움, 더 빠르면 PCLK3 (모듈 공장값 115200 은 처음 한 번 맞출 때만).
 * rak_ensure_baud() 가 io.set_baud 로 이 함수를 다시 불러 속도를 바꿈. */
HAL_StatusTypeDef lora_uart_init(lora_uart_t *u, uint32_t baud);
/* stm32h5xx_it.c 의 LPUART1_IRQHandler 에서 부름 */
void lora_uart_irq(lora_uart_t *u);
extern lora_uart_t g_lora_uart;
