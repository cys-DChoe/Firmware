#include "lowpower_stm32.h"
#include "lowpower.h"

RTC_HandleTypeDef g_rtc;
static volatile bool s_timer_fired;

HAL_StatusTypeDef lp_init(void)
{
    /* RTC 클럭 = LSE (백업 영역 쓰기 허용은 LSE 켤 때 이미 함) */
    RCC_PeriphCLKInitTypeDef clk = {0};
    clk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    clk.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK) {
        return HAL_ERROR;
    }
    __HAL_RCC_RTC_ENABLE();
    __HAL_RCC_RTCAPB_CLK_ENABLE();

    g_rtc.Instance = RTC;
    g_rtc.Init.HourFormat = RTC_HOURFORMAT_24;
    g_rtc.Init.AsynchPrediv = LP_RTC_PREDIV_A;
    g_rtc.Init.SynchPrediv = LP_RTC_PREDIV_S;
    g_rtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    g_rtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
    g_rtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    g_rtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
    g_rtc.Init.OutPutPullUp = RTC_OUTPUT_PULLUP_NONE;
    g_rtc.Init.BinMode = RTC_BINARY_NONE;
    if ((RTC->ICSR & RTC_ICSR_INITS) == 0u) {
        /* 처음 켤 때만 초기화. 이미 달력이 돌고 있으면(MCU 만 리셋) 맞춰 둔 시각을 지킴 */
        if (HAL_RTC_Init(&g_rtc) != HAL_OK) {
            return HAL_ERROR;
        }
    } else {
        g_rtc.State = HAL_RTC_STATE_READY;
    }
    HAL_NVIC_SetPriority(RTC_IRQn, 4, 0);
    HAL_NVIC_EnableIRQ(RTC_IRQn);

    /* RTC 깨우기가 Stop 을 깨우도록 EXTI 선 열기 — H5 에서 RTC 는 EXTI 17번 [가정: RM0481 EXTI 표, 보드에서 확인] */
    EXTI->IMR1 |= EXTI_IMR1_IM17;

    /* Stop 중 전압을 가장 낮게(SVOS3) · 내장 플래시 끔 → 잠 전류 줄이기 */
    if (HAL_PWREx_ControlStopModeVoltageScaling(PWR_REGULATOR_SVOS_SCALE3) != HAL_OK) {
        return HAL_ERROR;
    }
    HAL_PWREx_EnableFlashPowerDown();

#ifdef LP_DEBUG_STOP
    /* 디버거를 붙인 채로 잠을 볼 때만: 잠 중에도 디버그 연결 유지(전류는 늘어남) */
    HAL_DBGMCU_EnableDBGStopMode();
#endif
    return HAL_OK;
}

void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc)
{
    (void)hrtc;
    s_timer_fired = true;
}

lp_wake_t lp_sleep_ms(uint32_t ms)
{
    uint32_t reload;
    if (!lp_wut_reload(ms, &reload)) {
        return LP_WAKE_OTHER;
    }
    s_timer_fired = false;
    if (HAL_RTCEx_SetWakeUpTimer_IT(&g_rtc, reload, RTC_WAKEUPCLOCK_RTCCLK_DIV16, 0) != HAL_OK) {
        return LP_WAKE_OTHER;
    }
    HAL_SuspendTick();                                  /* SysTick 이 1 ms 마다 깨우지 않게 */
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
    /* 깨면 시스템 클럭은 HSI 또는 CSI(설정 따라, DS14258 3.10) — 1-Wire µs 지연이 SystemCoreClock 을 쓰므로 다시 읽음 (점2) */
    SystemCoreClockUpdate();
    HAL_ResumeTick();
    HAL_RTCEx_DeactivateWakeUpTimer(&g_rtc);
    /* Stop 중엔 RTC 그림자 레지스터(시각 읽는 칸)가 안 따라옴 → 깬 직후 읽으면 잠들기 전 시각이 나올 수 있음.
     * RSF 를 지우고 다시 맞춰질 때까지(RTCCLK 2번, ~61 µs) 기다림 (RM0481 RTC 'Reading the calendar' · ST 예제) */
    __HAL_RTC_WRITEPROTECTION_DISABLE(&g_rtc);
    (void)HAL_RTC_WaitForSynchro(&g_rtc);
    __HAL_RTC_WRITEPROTECTION_ENABLE(&g_rtc);
    return s_timer_fired ? LP_WAKE_TIMER : LP_WAKE_OTHER;
}

uint32_t lp_time_unix(void)
{
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;
    /* 시간을 먼저, 날짜를 뒤에 읽어야 그림자 레지스터가 풀림 */
    HAL_RTC_GetTime(&g_rtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&g_rtc, &d, RTC_FORMAT_BIN);
    const lp_civil_t c = {(uint16_t)(2000u + d.Year), d.Month, d.Date, t.Hours, t.Minutes, t.Seconds};
    return lp_unix_from_civil(&c);
}

uint32_t lp_time_ms(void)
{
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;
    HAL_RTC_GetTime(&g_rtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&g_rtc, &d, RTC_FORMAT_BIN);
    const lp_civil_t c = {(uint16_t)(2000u + d.Year), d.Month, d.Date, t.Hours, t.Minutes, t.Seconds};
    /* 아래 초(SubSeconds)는 SecondFraction 에서 0 으로 내려감 */
    const uint32_t frac = t.SecondFraction + 1u;
    const uint32_t sub_ms = (frac - 1u - t.SubSeconds) * 1000u / frac;
    return lp_unix_from_civil(&c) * 1000u + sub_ms;
}

HAL_StatusTypeDef lp_set_time_unix(uint32_t ts)
{
    lp_civil_t c;
    lp_civil_from_unix(ts, &c);
    if (c.year < 2000u || c.year > 2099u) {
        return HAL_ERROR;
    }
    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};
    t.Hours = c.hour;
    t.Minutes = c.min;
    t.Seconds = c.sec;
    d.Year = (uint8_t)(c.year - 2000u);
    d.Month = c.month;
    d.Date = c.day;
    d.WeekDay = (uint8_t)(((ts / 86400u) + 3u) % 7u + 1u);   /* 1970-01-01 = 목요일, RTC 는 월 = 1 */
    if (HAL_RTC_SetTime(&g_rtc, &t, RTC_FORMAT_BIN) != HAL_OK) {
        return HAL_ERROR;
    }
    return HAL_RTC_SetDate(&g_rtc, &d, RTC_FORMAT_BIN);
}
