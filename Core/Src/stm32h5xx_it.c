/* 예외 · 인터럽트 처리 */
#include "main.h"
#include "lora_uart_stm32.h"
#include "lowpower_stm32.h"
#include "nv_edata.h"
#include "wdg.h"

void NMI_Handler(void)
{
    /* 내장 EDATA 의 안 쓴 칸 · 쓰다 끊긴 칸을 읽으면 ECC 이중 오류로 여기 옴 → 지우고 돌아감 (nv_edata.h) */
    if (nv_edata_nmi()) {
        return;
    }
    wdg_fault(WDG_FAULT_NMI, 0u, 0u);
}

/* HardFault: 멈춘 곳(PC)과 원인(CFSR)을 백업 칸에 적고 재시작. 디버거가 붙어 있으면 빨강 LED 켜고 멈춤 (wdg.h).
 * 쌓인 레지스터에서 PC 를 꺼내려고 앞부분은 어셈블리 — 예외 직전 스택(MSP/PSP)을 r0 로 넘김. */
void hardfault_c(const uint32_t *frame)
{
    wdg_fault(WDG_FAULT_HARD, frame[6], SCB->CFSR);    /* frame: r0 r1 r2 r3 r12 lr pc xpsr */
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b hardfault_c   \n");
}

void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void SecureFault_Handler(void) __attribute__((alias("HardFault_Handler")));

void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void LPUART1_IRQHandler(void)
{
    lora_uart_irq(&g_lora_uart);
}

void RTC_IRQHandler(void)
{
    HAL_RTCEx_WakeUpTimerIRQHandler(&g_rtc);
}

void EXTI13_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(BTN_PIN);
}
