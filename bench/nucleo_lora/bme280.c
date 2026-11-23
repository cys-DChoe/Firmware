#include "bme280.h"

#define REG_ID        0xD0u
#define REG_CAL       0x88u
#define REG_CTRL_HUM  0xF2u
#define REG_STATUS    0xF3u
#define REG_CTRL_MEAS 0xF4u
#define REG_DATA      0xF7u

static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t s16le(const uint8_t *p) { return (int16_t)u16le(p); }

static int reg_read(const i2c_bus_t *bus, uint8_t reg, uint8_t *buf, size_t n)
{
    return bus->write_read(bus->ctx, BME280_ADDR, &reg, 1u, buf, n);
}

static int reg_write(const i2c_bus_t *bus, uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = {reg, val};
    return bus->write(bus->ctx, BME280_ADDR, b, 2u);
}

sens_err_t bme280_init(const i2c_bus_t *bus, bme280_cal_t *cal)
{
    uint8_t id = 0;
    if (reg_read(bus, REG_ID, &id, 1u) != 0) {
        return SENS_E_BUS;
    }
    if (id != BME280_CHIP_ID) {
        return SENS_E_ID;
    }
    uint8_t c[24];
    if (reg_read(bus, REG_CAL, c, sizeof c) != 0) {
        return SENS_E_BUS;
    }
    cal->t1 = u16le(&c[0]);
    cal->t2 = s16le(&c[2]);
    cal->t3 = s16le(&c[4]);
    cal->p1 = u16le(&c[6]);
    cal->p2 = s16le(&c[8]);
    cal->p3 = s16le(&c[10]);
    cal->p4 = s16le(&c[12]);
    cal->p5 = s16le(&c[14]);
    cal->p6 = s16le(&c[16]);
    cal->p7 = s16le(&c[18]);
    cal->p8 = s16le(&c[20]);
    cal->p9 = s16le(&c[22]);
    /* 습도 안 잼 (ctrl_hum 은 ctrl_meas 를 쓸 때 반영됨) */
    return reg_write(bus, REG_CTRL_HUM, 0x00u) == 0 ? SENS_OK : SENS_E_BUS;
}

sens_err_t bme280_measure(const i2c_bus_t *bus, const bme280_cal_t *cal, double *p_pa, double *t_c)
{
    /* 온도 ×1 · 기압 ×1 · 강제 모드 → 최대 약 9.3 ms (표 13) */
    if (reg_write(bus, REG_CTRL_MEAS, (uint8_t)((1u << 5) | (1u << 2) | 1u)) != 0) {
        return SENS_E_BUS;
    }
    bus->delay_ms(bus->ctx, 10u);
    uint8_t st = 0;
    for (int i = 0; i < 5; i++) {
        if (reg_read(bus, REG_STATUS, &st, 1u) != 0) {
            return SENS_E_BUS;
        }
        if ((st & 0x08u) == 0u) {      /* measuring 비트 */
            break;
        }
        bus->delay_ms(bus->ctx, 2u);
    }
    uint8_t d[6];
    if (reg_read(bus, REG_DATA, d, sizeof d) != 0) {
        return SENS_E_BUS;
    }
    const int32_t adc_p = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4));
    const int32_t adc_t = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4));
    if (adc_p == 0x80000 || adc_t == 0x80000) {   /* 건너뛴 값 */
        return SENS_E_BUSY;
    }

    /* 4.2.3 실수 보정식 */
    double v1 = ((double)adc_t / 16384.0 - (double)cal->t1 / 1024.0) * (double)cal->t2;
    double v2 = ((double)adc_t / 131072.0 - (double)cal->t1 / 8192.0);
    v2 = v2 * v2 * (double)cal->t3;
    const double t_fine = v1 + v2;
    *t_c = t_fine / 5120.0;

    v1 = t_fine / 2.0 - 64000.0;
    v2 = v1 * v1 * (double)cal->p6 / 32768.0;
    v2 = v2 + v1 * (double)cal->p5 * 2.0;
    v2 = v2 / 4.0 + (double)cal->p4 * 65536.0;
    v1 = ((double)cal->p3 * v1 * v1 / 524288.0 + (double)cal->p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * (double)cal->p1;
    if (v1 == 0.0) {
        return SENS_E_RANGE;
    }
    double p = 1048576.0 - (double)adc_p;
    p = (p - v2 / 4096.0) * 6250.0 / v1;
    v1 = (double)cal->p9 * p * p / 2147483648.0;
    v2 = p * (double)cal->p8 / 32768.0;
    *p_pa = p + (v1 + v2 + (double)cal->p7) / 16.0;
    return SENS_OK;
}
