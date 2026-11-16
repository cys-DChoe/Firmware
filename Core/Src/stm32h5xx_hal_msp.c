/* HAL 이 주변장치를 켤 때 부르는 핀 · 클럭 설정 */
#include "main.h"

void HAL_MspInit(void)
{
    /* 아직 할 것 없음 (전원 · 클럭 공통 설정 자리) */
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        /* 로그: USART3 — PD8 TX / PD9 RX, AF7 (pins.h DBG_TX · DBG_RX) */
        __HAL_RCC_USART3_CLK_ENABLE();
        __HAL_RCC_GPIOD_CLK_ENABLE();

        GPIO_InitTypeDef gpio = {0};
        gpio.Pin = DBG_TX_PIN | DBG_RX_PIN;
        gpio.Mode = GPIO_MODE_AF_PP;
        gpio.Pull = GPIO_PULLUP;
        gpio.Speed = GPIO_SPEED_FREQ_LOW;
        gpio.Alternate = GPIO_AF7_USART3;
        HAL_GPIO_Init(DBG_TX_PORT, &gpio);
    }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        __HAL_RCC_USART3_CLK_DISABLE();
        HAL_GPIO_DeInit(DBG_TX_PORT, DBG_TX_PIN | DBG_RX_PIN);
    }
}
