/* 보드 쪽 잠 모드 — RTC(LSE) 시각 · 깨우기 타이머 · Stop 들어가기
 * Stop 중 살아 있는 것: LSE · RTC · LPUART1(LSE, 9600) · 버튼 EXTI. SRAM 내용은 그대로.
 * 깨면 시스템 클럭은 HSI(리셋 기본과 같음, STOPWUCK = 0)라 다시 맞출 것이 없음. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "stm32h5xx_hal.h"

typedef enum {
    LP_WAKE_TIMER = 1,     /* 정한 시간이 지남 */
    LP_WAKE_OTHER = 2,     /* LoRa 글자 · 버튼 등 다른 인터럽트 */
} lp_wake_t;

HAL_StatusTypeDef lp_init(void);                  /* LSE 켜진 뒤 부름: RTC · Stop 설정 */
lp_wake_t lp_sleep_ms(uint32_t ms);               /* ms 동안 Stop (최대 32 s), 일찍 깨면 LP_WAKE_OTHER */
uint32_t lp_time_unix(void);
uint32_t lp_time_ms(void);                        /* RTC 시각을 ms 로 (32비트라 49일마다 돌아감 — 차이로만 씀, 눈금 약 4 ms) */
HAL_StatusTypeDef lp_set_time_unix(uint32_t t);   /* LoRa 망 시각 등으로 맞출 때 */
extern RTC_HandleTypeDef g_rtc;
