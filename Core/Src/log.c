#include <errno.h>
#include <string.h>
#include "log.h"

static UART_HandleTypeDef huart3;

HAL_StatusTypeDef log_init(void)
{
    huart3.Instance = USART3;
    huart3.Init.BaudRate = LOG_BAUD;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    return HAL_UART_Init(&huart3);
}

void log_write(const char *buf, size_t len)
{
    if (huart3.Instance == NULL || len == 0) {
        return;
    }
    HAL_UART_Transmit(&huart3, (const uint8_t *)buf, (uint16_t)len, 100);
}

/* newlib printf → USART3. "\n" 앞에 "\r" 을 붙여 터미널에서 줄이 바로 섬. */
int _write(int fd, const char *ptr, int len)
{
    if (fd != 1 && fd != 2) {
        errno = EBADF;
        return -1;
    }
    const char *start = ptr;
    for (int i = 0; i < len; i++) {
        if (ptr[i] == '\n') {
            log_write(start, (size_t)(&ptr[i] - start));
            log_write("\r\n", 2);
            start = &ptr[i + 1];
        }
    }
    log_write(start, (size_t)(&ptr[len] - start));
    return len;
}
