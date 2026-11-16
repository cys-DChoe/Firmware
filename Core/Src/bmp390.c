#include "bmp390.h"

#define REG_CHIP_ID   0x00u
#define REG_ERR       0x02u
#define REG_STATUS    0x03u
#define REG_DATA      0x04u
#define REG_PWR_CTRL  0x1Bu
#define REG_OSR       0x1Cu
#define REG_CONFIG    0x1Fu
#define REG_CALIB     0x31u
#define REG_CMD       0x7Eu

#define ST_DRDY_P     (1u << 5)
#define ST_DRDY_T     (1u << 6)

static int rd(const i2c_bus_t *bus, uint8_t reg, uint8_t *buf, size_t n)
{
    return bus->write_read(bus->ctx, BMP390_ADDR, &reg, 1, buf, n);
}

static int wr(const i2c_bus_t *bus, uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = {reg, val};
    return bus->write(bus->ctx, BMP390_ADDR, b, 2);
}

static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Bosch bmp3.c parse_calib_data 와 같은 눈금 */
void bmp390_parse_calib(const uint8_t n[21], bmp390_calib_t *c)
{
    c->t1 = (double)u16le(&n[0]) / 0.00390625;                 /* ÷ 2^−8 */
    c->t2 = (double)u16le(&n[2]) / 1073741824.0;               /* ÷ 2^30 */
    c->t3 = (double)(int8_t)n[4] / 281474976710656.0;          /* ÷ 2^48 */
    c->p1 = ((double)(int16_t)u16le(&n[5]) - 16384.0) / 1048576.0;     /* ÷ 2^20 */
    c->p2 = ((double)(int16_t)u16le(&n[7]) - 16384.0) / 536870912.0;   /* ÷ 2^29 */
    c->p3 = (double)(int8_t)n[9] / 4294967296.0;               /* ÷ 2^32 */
    c->p4 = (double)(int8_t)n[10] / 137438953472.0;            /* ÷ 2^37 */
    c->p5 = (double)u16le(&n[11]) / 0.125;                     /* ÷ 2^−3 */
    c->p6 = (double)u16le(&n[13]) / 64.0;                      /* ÷ 2^6 */
    c->p7 = (double)(int8_t)n[15] / 256.0;                     /* ÷ 2^8 */
    c->p8 = (double)(int8_t)n[16] / 32768.0;                   /* ÷ 2^15 */
    c->p9 = (double)(int16_t)u16le(&n[17]) / 281474976710656.0;    /* ÷ 2^48 */
    c->p10 = (double)(int8_t)n[19] / 281474976710656.0;        /* ÷ 2^48 */
    c->p11 = (double)(int8_t)n[20] / 36893488147419103232.0;   /* ÷ 2^65 */
}

sens_err_t bmp390_compensate(const bmp390_calib_t *c, uint32_t p_raw, uint32_t t_raw, double *p_pa, double *t_c)
{
    sens_err_t e = SENS_OK;

    /* 온도 */
    const double d1 = (double)t_raw - c->t1;
    double t = d1 * c->t2 + d1 * d1 * c->t3;
    if (t < -40.0) { t = -40.0; e = SENS_E_RANGE; }
    if (t > 85.0) { t = 85.0; e = SENS_E_RANGE; }

    /* 기압 */
    const double up = (double)p_raw;
    const double out1 = c->p5 + c->p6 * t + c->p7 * t * t + c->p8 * t * t * t;
    const double out2 = up * (c->p1 + c->p2 * t + c->p3 * t * t + c->p4 * t * t * t);
    const double out3 = up * up * (c->p9 + c->p10 * t) + up * up * up * c->p11;
    double p = out1 + out2 + out3;
    if (p < 30000.0) { p = 30000.0; e = SENS_E_RANGE; }
    if (p > 125000.0) { p = 125000.0; e = SENS_E_RANGE; }

    *t_c = t;
    *p_pa = p;
    return e;
}

uint32_t bmp390_conv_time_us(uint8_t osr_p, uint8_t osr_t)
{
    return 234u + (392u + (1u << osr_p) * 2020u) + (163u + (1u << osr_t) * 2020u);
}

sens_err_t bmp390_init(const i2c_bus_t *bus, bmp390_t *dev)
{
    uint8_t id = 0;
    if (rd(bus, REG_CHIP_ID, &id, 1) != 0) {
        return SENS_E_BUS;
    }
    if (id != BMP390_CHIP_ID) {
        return SENS_E_ID;
    }
    if (wr(bus, REG_CMD, 0xB6) != 0) {             /* 다시 시작 */
        return SENS_E_BUS;
    }
    bus->delay_ms(bus->ctx, BMP390_STARTUP_MS + 1u);

    uint8_t nvm[21];
    if (rd(bus, REG_CALIB, nvm, sizeof(nvm)) != 0) {
        return SENS_E_BUS;
    }
    bmp390_parse_calib(nvm, &dev->cal);

    dev->osr_p = 2;     /* ×4 — 데이터시트 "standard resolution" */
    dev->osr_t = 0;     /* ×1 */
    if (wr(bus, REG_OSR, (uint8_t)((dev->osr_t << 3) | dev->osr_p)) != 0 ||
        wr(bus, REG_CONFIG, 0x00) != 0) {          /* IIR 필터 끔 — 평균은 계산 모듈이 함 */
        return SENS_E_BUS;
    }
    uint8_t err = 0;
    if (rd(bus, REG_ERR, &err, 1) != 0) {
        return SENS_E_BUS;
    }
    return (err & 0x07u) ? SENS_E_STATUS : SENS_OK;    /* fatal · cmd · conf 오류 */
}

sens_err_t bmp390_measure(const i2c_bus_t *bus, const bmp390_t *dev, bmp390_reading_t *out)
{
    /* PWR_CTRL: press_en · temp_en · mode = 01(강제) */
    if (wr(bus, REG_PWR_CTRL, 0x13) != 0) {
        return SENS_E_BUS;
    }
    bus->delay_ms(bus->ctx, bmp390_conv_time_us(dev->osr_p, dev->osr_t) / 1000u + 1u);

    uint8_t st = 0;
    for (int tries = 0; ; tries++) {
        if (rd(bus, REG_STATUS, &st, 1) != 0) {
            return SENS_E_BUS;
        }
        if ((st & (ST_DRDY_P | ST_DRDY_T)) == (ST_DRDY_P | ST_DRDY_T)) {
            break;
        }
        if (tries >= 5) {
            return SENS_E_BUSY;
        }
        bus->delay_ms(bus->ctx, 1);
    }

    uint8_t d[6];
    if (rd(bus, REG_DATA, d, sizeof(d)) != 0) {
        return SENS_E_BUS;
    }
    out->p_raw = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16);
    out->t_raw = (uint32_t)d[3] | ((uint32_t)d[4] << 8) | ((uint32_t)d[5] << 16);
    return bmp390_compensate(&dev->cal, out->p_raw, out->t_raw, &out->p_pa, &out->t_c);
}
