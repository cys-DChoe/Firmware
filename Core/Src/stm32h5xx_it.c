/* 예외 · 인터럽트 처리 */
#include "main.h"

void NMI_Handler(void)
{
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
