/* 보드 I2C — I2C2(HSC, PB10/PB12) · I2C3(ABP2 · BMP280 · SDP810, PA8/PC9), 100 kHz, 외부 풀업 4.7 kΩ */
#pragma once

#include "i2c_bus.h"
#include "stm32h5xx_hal.h"

#define I2C_FAILS_REOPEN  3u   /* 이만큼 이어서 실패하면 버스를 다시 엶 */

typedef struct {
    I2C_HandleTypeDef h;
    i2c_bus_t bus;
    uint8_t fails;       /* 연속 실패 */
    uint32_t reopens;    /* 다시 연 횟수 (로그용) */
} i2c_stm32_t;

/* inst = I2C2 또는 I2C3. 100 kHz 타이밍은 지금 버스 클럭에서 계산함. */
HAL_StatusTypeDef i2c_stm32_init(i2c_stm32_t *p, I2C_TypeDef *inst);
/* 버스를 껐다 다시 엶 (PE 를 내렸다 올려 굳은 BUSY 를 풂).
 * 센서 전원을 껐다 켜면 선이 같이 내려갔다 올라오면서 MCU 가 "버스 사용 중"으로 굳을 수 있음 → 켠 뒤 한 번 부름.
 * 그 밖에도 I2C_FAILS_REOPEN 번 이어서 실패하면 스스로 부름. */
void i2c_stm32_reopen(i2c_stm32_t *p);
