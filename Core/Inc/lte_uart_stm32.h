/* 보드 쪽 LTE 모듈 연결 — USART6 (PG14 TX → MAIN_RXD, PG9 RX ← MAIN_TXD, AF7, 1.8 V VDDIO2)
 * PWRKEY PB5 · RESET PG15 (NPN, 높음 = 누름) · STATUS PB9 (1.8 V 입력)
 * 1.8 V 핀(PG9–PG14 · PB8 · PB9)은 모듈 VDD_EXT 가 MCU VDDIO2 로 들어와야 살아남 → io_on 에서 PWR VMSR.VDDIO2RDY 를 보고 켬.
 * DTR(PB8) · RI(PG11) · RTS/CTS(PG12/PG13)는 1차에서 안 씀 — 켤 때 그대로(아날로그) 둠 (펌웨어 주의 5절) */
#pragma once

#include <stdbool.h>
#include "lte.h"
#include "stm32h5xx_hal.h"

#define LTE_RX_BUF  512u     /* 2의 거듭제곱 */
#define LTE_BAUD    115200u  /* 모듈 공장값 (Hardware Design 4.x) */

typedef struct {
    UART_HandleTypeDef h;
    volatile uint16_t head, tail;
    volatile uint32_t overruns;
    uint8_t buf[LTE_RX_BUF];
    bool up;
    lte_io_t io;
} lte_uart_t;

void lte_board_init(lte_uart_t *u);   /* STATUS 입력 설정 · io 묶기 (UART 는 모듈이 켜진 뒤 io_on 에서) */
void lte_uart_irq(lte_uart_t *u);     /* stm32h5xx_it.c USART6_IRQHandler */
extern lte_uart_t g_lte_uart;
