#include "i2c_timing.h"

uint32_t i2c_timing_100k(uint32_t kernel_hz)
{
    /* tPRESC = (PRESC + 1) / I2CCLK 를 250 ns 이상으로 — 클럭이 4 MHz 배수가 아니면 올림(조금 느려짐) */
    uint32_t presc = (kernel_hz + 3999999u) / 4000000u;
    if (presc > 0u) {
        presc -= 1u;
    }
    if (presc > 15u) {
        presc = 15u;
    }
    return (presc << 28) | (0x4u << 20) | (0x2u << 16) | (0x0Fu << 8) | 0x13u;
}
