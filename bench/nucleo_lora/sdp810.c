#include "sdp810.h"

static uint8_t crc8(const uint8_t *p)
{
    uint8_t crc = 0xFFu;
    for (int i = 0; i < 2; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

sens_err_t sdp810_start(const i2c_bus_t *bus)
{
    const uint8_t stop[2] = {0x3Fu, 0xF9u};
    const uint8_t start[2] = {0x36u, 0x15u};
    (void)bus->write(bus->ctx, SDP810_ADDR, stop, 2u);   /* 안 돌고 있었으면 답이 없을 수 있음 — 괜찮음 */
    bus->delay_ms(bus->ctx, 2u);
    if (bus->write(bus->ctx, SDP810_ADDR, start, 2u) != 0) {
        return SENS_E_BUS;
    }
    bus->delay_ms(bus->ctx, 10u);
    return SENS_OK;
}

sens_err_t sdp810_read(const i2c_bus_t *bus, double *dp_pa, double *t_c)
{
    uint8_t d[9];
    if (bus->read(bus->ctx, SDP810_ADDR, d, sizeof d) != 0) {
        return SENS_E_BUS;
    }
    if (crc8(&d[0]) != d[2] || crc8(&d[3]) != d[5] || crc8(&d[6]) != d[8]) {
        return SENS_E_STATUS;
    }
    const int16_t dp = (int16_t)((d[0] << 8) | d[1]);
    const int16_t t = (int16_t)((d[3] << 8) | d[4]);
    const int16_t scale = (int16_t)((d[6] << 8) | d[7]);
    if (scale == 0) {
        return SENS_E_STATUS;
    }
    *dp_pa = (double)dp / (double)scale;
    *t_c = (double)t / 200.0;
    return SENS_OK;
}
