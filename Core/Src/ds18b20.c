/* DS18B20 — 1-Wire 바이트 · CRC 와 변환 · 읽기 (ds18b20.h 머리말) */
#include "ds18b20.h"

#define CMD_SKIP_ROM   0xCCu
#define CMD_CONVERT    0x44u
#define CMD_READ_PAD   0xBEu

void onewire_write_byte(const onewire_t *ow, uint8_t v)
{
    for (int i = 0; i < 8; i++) {
        ow->write_bit(ow->ctx, ((v >> i) & 1u) != 0u);
    }
}

uint8_t onewire_read_byte(const onewire_t *ow)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (ow->read_bit(ow->ctx)) {
            v |= (uint8_t)(1u << i);
        }
    }
    return v;
}

uint8_t onewire_crc8(const uint8_t *p, int n)
{
    uint8_t crc = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = p[i];
        for (int k = 0; k < 8; k++) {
            const uint8_t mix = (uint8_t)((crc ^ b) & 1u);
            crc >>= 1;
            if (mix) {
                crc ^= 0x8Cu;
            }
            b >>= 1;
        }
    }
    return crc;
}

sens_err_t ds18b20_convert(const onewire_t *ow)
{
    if (!ow->reset(ow->ctx)) {
        return SENS_E_BUS;
    }
    onewire_write_byte(ow, CMD_SKIP_ROM);
    onewire_write_byte(ow, CMD_CONVERT);
    return SENS_OK;
}

sens_err_t ds18b20_read(const onewire_t *ow, double *t_c)
{
    if (!ow->reset(ow->ctx)) {
        return SENS_E_BUS;
    }
    onewire_write_byte(ow, CMD_SKIP_ROM);
    onewire_write_byte(ow, CMD_READ_PAD);
    uint8_t d[9];
    bool all_zero = true, all_ff = true;
    for (int i = 0; i < 9; i++) {
        d[i] = onewire_read_byte(ow);
        all_zero = all_zero && d[i] == 0x00u;
        all_ff = all_ff && d[i] == 0xFFu;
    }
    /* 전부 0(선이 낮게 붙음)은 CRC 도 0 이라 통과해 버림 · 전부 FF 는 답 없음 — 둘 다 걸러 냄 */
    if (all_zero || all_ff || onewire_crc8(d, 8) != d[8]) {
        return SENS_E_STATUS;
    }
    const double t = (double)(int16_t)((d[1] << 8) | d[0]) / 16.0;
    *t_c = t;
    return (t == DS18B20_POWERON_C) ? SENS_STALE : SENS_OK;
}
