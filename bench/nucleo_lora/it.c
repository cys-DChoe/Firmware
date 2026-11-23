/* Nucleo 벤치 인터럽트 — 보드 펌웨어(Core/Src/stm32h5xx_it.c)는 LTE · 잠 · EDATA 까지 끌고 와서 따로 둠 */
#include "stm32h5xx_hal.h"
#include "lora_uart_stm32.h"
#include "wdg.h"

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void LPUART1_IRQHandler(void)
{
    lora_uart_irq(&g_lora_uart);
}

void HardFault_Handler(void)
{
    for (;;) {
    }
}

void NMI_Handler(void) {}
void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}

/* syscalls.c 의 _exit 이 부름 — 벤치는 그냥 다시 시작 */
void wdg_fault(uint32_t code, uint32_t info, uint32_t cfsr)
{
    (void)code;
    (void)info;
    (void)cfsr;
    NVIC_SystemReset();
    for (;;) {
    }
}
