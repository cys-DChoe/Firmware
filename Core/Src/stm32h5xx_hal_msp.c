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
    } else if (huart->Instance == LPUART1) {
        /* LoRa: PB6 LPUART1_TX → RAK3172 UART2_RX · PB7 LPUART1_RX ← UART2_TX, AF8 (pins.h MODEM_RX · MODEM_TX) */
        __HAL_RCC_LPUART1_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();

        GPIO_InitTypeDef gpio = {0};
        gpio.Pin = MODEM_RX_PIN | MODEM_TX_PIN;
        gpio.Mode = GPIO_MODE_AF_PP;
        gpio.Pull = GPIO_PULLUP;            /* 모듈이 꺼져 있을 때 받는 선이 떠서 잡음 글자가 들어오지 않게 */
        gpio.Speed = GPIO_SPEED_FREQ_LOW;
        gpio.Alternate = GPIO_AF8_LPUART1;
        HAL_GPIO_Init(MODEM_RX_PORT, &gpio);
    }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        __HAL_RCC_USART3_CLK_DISABLE();
        HAL_GPIO_DeInit(DBG_TX_PORT, DBG_TX_PIN | DBG_RX_PIN);
    } else if (huart->Instance == LPUART1) {
        HAL_NVIC_DisableIRQ(LPUART1_IRQn);
        __HAL_RCC_LPUART1_CLK_DISABLE();
        HAL_GPIO_DeInit(MODEM_RX_PORT, MODEM_RX_PIN | MODEM_TX_PIN);
    }
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    /* 외부 풀업 4.7 kΩ 이 있어 내부 풀업은 안 씀. 오픈 드레인 · AF4. */
    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    if (hi2c->Instance == I2C2) {
        /* HSC 차압: PB10 SCL · PB12 SDA */
        __HAL_RCC_I2C2_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        gpio.Alternate = GPIO_AF4_I2C2;
        gpio.Pin = I2C2_SCL_PIN | I2C2_SDA_PIN;
        HAL_GPIO_Init(I2C2_SCL_PORT, &gpio);
    } else if (hi2c->Instance == I2C3) {
        /* ABP2 정압 · BMP390 기압: PA8 SCL · PC9 SDA */
        __HAL_RCC_I2C3_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOC_CLK_ENABLE();
        gpio.Alternate = GPIO_AF4_I2C3;
        gpio.Pin = I2C3_SCL_PIN;
        HAL_GPIO_Init(I2C3_SCL_PORT, &gpio);
        gpio.Pin = I2C3_SDA_PIN;
        HAL_GPIO_Init(I2C3_SDA_PORT, &gpio);
    }
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C2) {
        __HAL_RCC_I2C2_CLK_DISABLE();
        HAL_GPIO_DeInit(I2C2_SCL_PORT, I2C2_SCL_PIN | I2C2_SDA_PIN);
    } else if (hi2c->Instance == I2C3) {
        __HAL_RCC_I2C3_CLK_DISABLE();
        HAL_GPIO_DeInit(I2C3_SCL_PORT, I2C3_SCL_PIN);
        HAL_GPIO_DeInit(I2C3_SDA_PORT, I2C3_SDA_PIN);
    }
}

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1) {
        /* 외부 플래시 W25Q128: PA5 SCK · PA6 MISO · PA7 MOSI, AF5 */
        __HAL_RCC_SPI1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        GPIO_InitTypeDef gpio = {0};
        gpio.Pin = SPI1_SCK_PIN | SPI1_MISO_PIN | SPI1_MOSI_PIN;
        gpio.Mode = GPIO_MODE_AF_PP;
        gpio.Pull = GPIO_NOPULL;
        gpio.Speed = GPIO_SPEED_FREQ_MEDIUM;
        gpio.Alternate = GPIO_AF5_SPI1;
        HAL_GPIO_Init(SPI1_SCK_PORT, &gpio);
    }
}

void HAL_SPI_MspDeInit(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1) {
        __HAL_RCC_SPI1_CLK_DISABLE();
        HAL_GPIO_DeInit(SPI1_SCK_PORT, SPI1_SCK_PIN | SPI1_MISO_PIN | SPI1_MOSI_PIN);
    }
}
