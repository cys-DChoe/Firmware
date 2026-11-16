/* 잠 모드 계산 부분 (보드 없이 맥에서 시험)
 *
 * 설계서 결정: ① 조리 중(흐름 있음) 1초 · 평소 10초마다 깸 ⑦ Stop + LSE, LoRa UART 9600
 *   - 깨우는 시계: RTC 깨우기 타이머, LSE 32.768 kHz ÷ 16 = 2048 Hz → 1틱 0.49 ms, 최대 65536틱 = 32 s
 *   - 시각: RTC 달력(LSE ÷ 128 ÷ 256 = 1 Hz) ↔ Unix 초
 *   - LoRa UART: LPUART1 을 LSE 로 돌리면 Stop 중에도 받음. LPUART 는 커널 클럭이 보드레이트의 3배 이상,
 *     BRR = 256 × 커널 / 보드레이트 는 0x300 이상이어야 함 → LSE 로 9600 은 되고 115200 은 안 됨
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LP_LSE_HZ          32768u
#define LP_WUT_HZ          (LP_LSE_HZ / 16u)      /* 2048 */
#define LP_RTC_PREDIV_A    127u                    /* 32768 / 128 = 256 Hz */
#define LP_RTC_PREDIV_S    255u                    /* 256 / 256 = 1 Hz */
#define LP_TICK_ACTIVE_MS  1000u
#define LP_TICK_IDLE_MS    10000u

/* 다음에 깰 때까지 (흐름 있으면 1 s, 없으면 10 s) */
uint32_t lp_tick_ms(bool flow_active);
/* RTC 깨우기 타이머 값(WUTR). ms 가 0 이거나 32 s 를 넘으면 false */
bool lp_wut_reload(uint32_t ms, uint32_t *reload);
/* 그 값으로 실제 자는 시간 (µs) — 반올림 오차 확인용 */
uint32_t lp_wut_actual_us(uint32_t reload);
/* LPUART BRR. 쓸 수 없는 조합이면 false. err_ppm 에 실제 속도 오차 */
bool lp_lpuart_brr(uint32_t kernel_hz, uint32_t baud, uint32_t *brr, int32_t *err_ppm);

/* 달력 ↔ Unix 초 (UTC, 2000–2099) */
typedef struct { uint16_t year; uint8_t month, day, hour, min, sec; } lp_civil_t;
uint32_t lp_unix_from_civil(const lp_civil_t *c);
void lp_civil_from_unix(uint32_t t, lp_civil_t *c);

/* 1분 경계를 넘었나: 지난 보고의 분 번호와 지금 Unix 초 */
bool lp_minute_due(uint32_t last_minute, uint32_t now_s);
