/* 보드 쪽 외부 플래시 연결 — SPI1 (PA5 SCK · PA6 MISO · PA7 MOSI, AF5) · CS PC4 (낮음 = 선택)
 * SPI1 커널 클럭은 리셋 기본이 PLL1Q(꺼져 있음)라 CLKP(HSI 32 MHz)로 바꾸고 /4 = 8 MHz. 읽기 03h 한도 50 MHz 안. */
#pragma once

#include "stm32h5xx_hal.h"
#include "w25q.h"

typedef struct {
    SPI_HandleTypeDef h;
    spi_dev_t dev;
} flash_spi_t;

HAL_StatusTypeDef flash_spi_init(flash_spi_t *f);
