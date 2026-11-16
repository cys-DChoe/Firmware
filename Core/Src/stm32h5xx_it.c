/* 예외 · 인터럽트 처리 */
#include "main.h"
#include "lora_uart_stm32.h"
#include "lowpower_stm32.h"
#include "nv_edata.h"

void NMI_Handler(void)
{
    /* 내장 EDATA 의 안 쓴 칸 · 쓰다 끊긴 칸을 읽으면 ECC 이중 오류로 여기 옴 → 지우고 돌아감 (nv_edata.h) */
    if (nv_edata_nmi()) {
        return;
    }
    while (1) {
    }
}

void HardFault_Handler(void)
{
    /* 디버거로 멈춰 보면 여기서 서 있음. LED2 를 켜 두어 눈으로도 알게 함. */
    HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_SET);
    while (1) {
    }
}

void MemManage_Handler(void) { HardFault_Handler(); }
void BusFault_Handler(void) { HardFault_Handler(); }
void UsageFault_Handler(void) { HardFault_Handler(); }
void SecureFault_Handler(void) { HardFault_Handler(); }

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
