/* 보드 I2C — I2C2(HSC, PB10/PB12) · I2C3(ABP2 · BMP390, PA8/PC9), 100 kHz, 외부 풀업 4.7 kΩ */
#pragma once

#include "i2c_bus.h"
#include "stm32h5xx_hal.h"

typedef struct {
    I2C_HandleTypeDef h;
    i2c_bus_t bus;
} i2c_stm32_t;

/* inst = I2C2 또는 I2C3. 100 kHz 타이밍은 지금 버스 클럭에서 계산함. */
HAL_StatusTypeDef i2c_stm32_init(i2c_stm32_t *p, I2C_TypeDef *inst);
