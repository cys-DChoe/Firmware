#include "hsc.h"

void hsc_default_cfg(hsc_cfg_t *cfg)
{
    cfg->p_min_pa = -1000.0;    /* HSCDRRN010MD2A3: −10 … +10 mbar */
    cfg->p_max_pa = 1000.0;
}

sens_err_t hsc_decode(const uint8_t raw[4], const hsc_cfg_t *cfg, hsc_reading_t *out)
{
    out->status = (uint8_t)(raw[0] >> 6);
    out->p_counts = (uint16_t)(((raw[0] & 0x3Fu) << 8) | raw[1]);
    out->t_counts = (uint16_t)((raw[2] << 3) | (raw[3] >> 5));

    /* 압력 = (출력 − 출력min) × (Pmax − Pmin) ÷ (출력max − 출력min) + Pmin */
    out->p_pa = ((double)out->p_counts - HSC_OUT_MIN) * (cfg->p_max_pa - cfg->p_min_pa) /
                    (HSC_OUT_MAX - HSC_OUT_MIN) + cfg->p_min_pa;
    out->t_c = (double)out->t_counts * 200.0 / 2047.0 - 50.0;

    switch (out->status) {
    case 0:
        break;
    case 2:
        return SENS_STALE;          /* 전에 준 값과 같음 — 너무 자주 읽었을 때 */
    default:
        return SENS_E_STATUS;       /* 01 명령 모드 · 11 진단 오류 */
    }
    /* 10–90 % 밖은 센서 범위 밖 (과압 등) — 값은 그대로 주고 알림 */
    if (out->p_counts < (uint16_t)HSC_OUT_MIN || out->p_counts > (uint16_t)HSC_OUT_MAX) {
        return SENS_E_RANGE;
    }
    return SENS_OK;
}

sens_err_t hsc_read(const i2c_bus_t *bus, const hsc_cfg_t *cfg, hsc_reading_t *out)
{
    uint8_t raw[4];
    if (bus->read(bus->ctx, HSC_ADDR, raw, sizeof(raw)) != 0) {
        return SENS_E_BUS;
    }
    return hsc_decode(raw, cfg, out);
}
