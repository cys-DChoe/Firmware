/* 로그 — USART3(PD8 TX / PD9 RX, J_DBG 헤더), 115200 8N1
 * 맥에서 보기: pyserial-miniterm /dev/cu.usbserial-XXXX 115200
 * printf 도 이쪽으로 나감.
 */
#pragma once

#include "stm32h5xx_hal.h"

#define LOG_BAUD 115200U

HAL_StatusTypeDef log_init(void);
void log_write(const char *buf, size_t len);
