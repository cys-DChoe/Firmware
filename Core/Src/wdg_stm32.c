/* 워치독 · 오류 재시작 — 설명은 wdg.h */
#include "wdg.h"
#include "main.h"

#define BKP_MAGIC        0x57D60000u   /* 위 16비트: 우리가 적은 칸이라는 표시 */
#define BKP_MAGIC_MASK   0xFFFF0000u
#define STREAK_STANDBY   3u

/* IWDG: LSI 32 kHz [RM0481 — 보드에서 확인] · PR 6 = ÷256 → 125 Hz · RLR 3749 → 30.0 s (최대 4095 = 32.8 s) */
#define IWDG_PR_DIV256   6u
#define IWDG_RELOAD      3749u

static void bkp_access(void)
{
    RCC->APB3ENR |= RCC_APB3ENR_RTCAPBEN;     /* TAMP 백업 칸은 RTC APB 시계로 읽고 씀 */
    (void)RCC->APB3ENR;
    PWR->DBPCR |= PWR_DBPCR_DBP;              /* 백업 도메인 쓰기 허용 */
}

static bool debugger_on(void)
{
    return (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0u;
}

wdg_boot_t wdg_boot_cause(void)
{
    wdg_boot_t b = {WDG_CAUSE_OTHER, 0u, 0u, 0u, 0u};
    bkp_access();
    const uint32_t rsr = RCC->RSR;
    const uint32_t tag = TAMP->BKP0R;

    /* 표식은 켤 때마다 지우므로, 남아 있으면 지난번 켜짐이 오류 처리로 끝난 것.
     * 3번 이어져 Standby 로 쉬었다 워치독이 깨운 경우(재시작 표시는 워치독)도 원인 · 주소를 그대로 읽음 */
    if ((tag & BKP_MAGIC_MASK) == BKP_MAGIC) {
        b.cause = WDG_CAUSE_FAULT;
        b.code = (uint8_t)(tag & 0xFFu);
        b.info = TAMP->BKP1R;
        b.cfsr = TAMP->BKP3R;
    } else if ((rsr & RCC_RSR_IWDGRSTF) != 0u) {
        b.cause = WDG_CAUSE_IWDG;
    } else if ((rsr & RCC_RSR_BORRSTF) != 0u) {
        b.cause = WDG_CAUSE_POWER;
    } else if ((rsr & RCC_RSR_SFTRSTF) != 0u) {
        b.cause = WDG_CAUSE_SOFT;
    } else if ((rsr & RCC_RSR_PINRSTF) != 0u) {   /* 다른 리셋도 핀 표시를 같이 켜므로 맨 뒤 */
        b.cause = WDG_CAUSE_PIN;
    }
    TAMP->BKP0R = 0u;
    RCC->RSR |= RCC_RSR_RMVF;                     /* 표시 지움 — 다음 재시작 때 새로 읽게 */

    if (wdg_abnormal(&b)) {
        TAMP->BKP2R = TAMP->BKP2R + 1u;
    } else {
        TAMP->BKP2R = 0u;
    }
    b.streak = TAMP->BKP2R;
    return b;
}

bool wdg_abnormal(const wdg_boot_t *b)
{
    return b->cause == WDG_CAUSE_IWDG || b->cause == WDG_CAUSE_FAULT;
}

const char *wdg_cause_name(wdg_cause_t c)
{
    switch (c) {
    case WDG_CAUSE_POWER: return "전원";
    case WDG_CAUSE_PIN:   return "리셋 핀";
    case WDG_CAUSE_IWDG:  return "워치독";
    case WDG_CAUSE_FAULT: return "오류";
    case WDG_CAUSE_SOFT:  return "소프트웨어";
    default:              return "모름";
    }
}

void wdg_start(void)
{
    DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP;   /* 디버거로 멈추면 워치독도 멈춤 */
    IWDG->KR = 0xCCCCu;                                   /* 켬 (LSI 도 같이 켜짐, 끌 수 없음) */
    IWDG->KR = 0x5555u;                                   /* 설정 잠금 풀기 */
    IWDG->PR = IWDG_PR_DIV256;
    IWDG->RLR = IWDG_RELOAD;
    while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU)) != 0u) {
    }
    IWDG->KR = 0xAAAAu;
}

void wdg_kick(void)
{
    IWDG->KR = 0xAAAAu;
}

void wdg_healthy(void)
{
    bkp_access();
    TAMP->BKP2R = 0u;
}

void wdg_fault(uint32_t code, uint32_t info, uint32_t cfsr)
{
    __disable_irq();
    HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_SET);
    if (debugger_on()) {
        while (1) {                 /* 디버거로 보려고 여기 멈춤 (워치독은 디버거가 멈출 때만 멈춤) */
        }
    }
    bkp_access();
    TAMP->BKP0R = BKP_MAGIC | (code & 0xFFu);
    TAMP->BKP1R = info;
    TAMP->BKP3R = cfsr;
    if (TAMP->BKP2R + 1u >= STREAK_STANDBY) {
        /* 이어서 3번째 — 바로 다시 켜지 않고 잠. 워치독이 30 s 뒤 재시작시킴 (Standby 에서 나오는 것도 재시작) */
        /* 걸려 있는 인터럽트(1 ms SysTick · LoRa 글자 등)가 하나라도 있으면 WFI 가 바로 돌아와 못 쉼 → 모두 끄고 지움 */
        SysTick->CTRL = 0u;
        SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
        for (uint32_t i = 0; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++) {
            NVIC->ICER[i] = 0xFFFFFFFFu;
            NVIC->ICPR[i] = 0xFFFFFFFFu;
        }
        HAL_PWR_EnterSTANDBYMode();
    }
    NVIC_SystemReset();
}
