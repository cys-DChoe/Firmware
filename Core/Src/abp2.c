#include "abp2.h"

void abp2_default_cfg(abp2_cfg_t *cfg)
{
    cfg->p_min_pa = 0.0;        /* ABP2DRNT006KG2A3XX: 0 … 6 kPa 게이지 */
    cfg->p_max_pa = 6000.0;
}

sens_err_t abp2_decode(const uint8_t raw[7], const abp2_cfg_t *cfg, abp2_reading_t *out)
{
    out->status = raw[0];
    out->p_counts = ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
    out->t_counts = ((uint32_t)raw[4] << 16) | ((uint32_t)raw[5] << 8) | raw[6];
    out->p_pa = ((double)out->p_counts - ABP2_OUT_MIN) * (cfg->p_max_pa - cfg->p_min_pa) /
                    (ABP2_OUT_MAX - ABP2_OUT_MIN) + cfg->p_min_pa;
    out->t_c = (double)out->t_counts * 200.0 / 16777215.0 - 50.0;

    if (out->status & ABP2_ST_BUSY) {
        return SENS_E_BUSY;
    }
    if (!(out->status & ABP2_ST_POWER) || (out->status & (ABP2_ST_MEMERR | ABP2_ST_SAT))) {
        return SENS_E_STATUS;
    }
    if (out->p_counts < (uint32_t)ABP2_OUT_MIN || out->p_counts > (uint32_t)ABP2_OUT_MAX) {
        return SENS_E_RANGE;
    }
    return SENS_OK;
}

sens_err_t abp2_start(const i2c_bus_t *bus)
{
    static const uint8_t cmd[3] = {0xAA, 0x00, 0x00};
    return bus->write(bus->ctx, ABP2_ADDR, cmd, sizeof(cmd)) == 0 ? SENS_OK : SENS_E_BUS;
}

sens_err_t abp2_fetch(const i2c_bus_t *bus, const abp2_cfg_t *cfg, abp2_reading_t *out)
{
    uint8_t raw[7];
    if (bus->read(bus->ctx, ABP2_ADDR, raw, sizeof(raw)) != 0) {
        return SENS_E_BUS;
    }
    return abp2_decode(raw, cfg, out);
}

sens_err_t abp2_measure(const i2c_bus_t *bus, const abp2_cfg_t *cfg, abp2_reading_t *out)
{
    sens_err_t e = abp2_start(bus);
    if (e != SENS_OK) {
        return e;
    }
    bus->delay_ms(bus->ctx, ABP2_CONV_MS);
    for (int tries = 0; tries < 5; tries++) {
        e = abp2_fetch(bus, cfg, out);
        if (e != SENS_E_BUSY) {
            return e;
        }
        bus->delay_ms(bus->ctx, 1);
    }
    return SENS_E_BUSY;
}
