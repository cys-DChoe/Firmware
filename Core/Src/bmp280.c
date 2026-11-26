#include "bmp280.h"

#define REG_CALIB     0x88u
#define REG_ID        0xD0u
#define REG_RESET     0xE0u
#define REG_STATUS    0xF3u
#define REG_CTRL_MEAS 0xF4u
#define REG_CONFIG    0xF5u
#define REG_DATA      0xF7u

#define ST_MEASURING  (1u << 3)
#define ST_IM_UPDATE  (1u << 0)

#define RAW_SKIPPED   0x80000     /* 측정을 건너뛴 칸 (표 4 · 5) */

static int rd(const i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    return bus->write_read(bus->ctx, addr, &reg, 1, buf, n);
}

static int wr(const i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = {reg, val};
    return bus->write(bus->ctx, addr, b, 2);
}

static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool id_ok(uint8_t id)
{
    return (id >= 0x56u && id <= BMP280_CHIP_ID) || id == BME280_CHIP_ID;
}

/* 표 17 */
void bmp280_parse_calib(const uint8_t n[24], bmp280_calib_t *c)
{
    c->t1 = u16le(&n[0]);
    c->t2 = (int16_t)u16le(&n[2]);
    c->t3 = (int16_t)u16le(&n[4]);
    c->p1 = u16le(&n[6]);
    c->p2 = (int16_t)u16le(&n[8]);
    c->p3 = (int16_t)u16le(&n[10]);
    c->p4 = (int16_t)u16le(&n[12]);
    c->p5 = (int16_t)u16le(&n[14]);
    c->p6 = (int16_t)u16le(&n[16]);
    c->p7 = (int16_t)u16le(&n[18]);
    c->p8 = (int16_t)u16le(&n[20]);
    c->p9 = (int16_t)u16le(&n[22]);
}

/* 데이터시트 8.1 실수식 그대로 (Bosch compensate_temperature · compensate_pressure 와 같음) */
sens_err_t bmp280_compensate(const bmp280_calib_t *c, int32_t p_raw, int32_t t_raw, double *p_pa, double *t_c)
{
    sens_err_t e = SENS_OK;

    /* 온도 */
    double v1 = ((double)t_raw / 16384.0 - (double)c->t1 / 1024.0) * (double)c->t2;
    double v2 = (double)t_raw / 131072.0 - (double)c->t1 / 8192.0;
    v2 = v2 * v2 * (double)c->t3;
    const int32_t t_fine = (int32_t)(v1 + v2);
    double t = (v1 + v2) / 5120.0;
    if (t < -40.0) { t = -40.0; e = SENS_E_RANGE; }
    if (t > 85.0) { t = 85.0; e = SENS_E_RANGE; }

    /* 기압 */
    double p = 30000.0;
    v1 = (double)t_fine / 2.0 - 64000.0;
    v2 = v1 * v1 * (double)c->p6 / 32768.0;
    v2 = v2 + v1 * (double)c->p5 * 2.0;
    v2 = v2 / 4.0 + (double)c->p4 * 65536.0;
    v1 = ((double)c->p3 * v1 * v1 / 524288.0 + (double)c->p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * (double)c->p1;
    if (v1 > 0.0) {                                 /* 0 으로 나누기 막음 (Bosch 와 같이 0 이하면 끝값) */
        p = 1048576.0 - (double)p_raw;
        p = (p - v2 / 4096.0) * 6250.0 / v1;
        v1 = (double)c->p9 * p * p / 2147483648.0;
        v2 = p * (double)c->p8 / 32768.0;
        p = p + (v1 + v2 + (double)c->p7) / 16.0;
    } else {
        e = SENS_E_RANGE;
    }
    if (p < 30000.0) { p = 30000.0; e = SENS_E_RANGE; }
    if (p > 110000.0) { p = 110000.0; e = SENS_E_RANGE; }

    *t_c = t;
    *p_pa = p;
    return e;
}

sens_err_t bmp280_init(const i2c_bus_t *bus, bmp280_t *dev)
{
    static const uint8_t addrs[2] = {BMP280_ADDR_LO, BMP280_ADDR_HI};
    sens_err_t e = SENS_E_BUS;
    dev->addr = 0;
    for (size_t i = 0; i < 2u && dev->addr == 0u; i++) {
        uint8_t id = 0;
        if (rd(bus, addrs[i], REG_ID, &id, 1) != 0) {
            continue;
        }
        if (id_ok(id)) {
            dev->addr = addrs[i];
            dev->chip_id = id;
        } else {
            e = SENS_E_ID;          /* 답은 했는데 다른 칩 */
        }
    }
    if (dev->addr == 0u) {
        return e;
    }
    const uint8_t a = dev->addr;

    if (wr(bus, a, REG_RESET, 0xB6u) != 0) {       /* 다시 시작 (전원 켤 때와 같은 순서) */
        return SENS_E_BUS;
    }
    bus->delay_ms(bus->ctx, BMP280_STARTUP_MS + 1u);
    for (int tries = 0; ; tries++) {                /* 보정값을 NVM 에서 옮기는 중이면 기다림 */
        uint8_t st = 0;
        if (rd(bus, a, REG_STATUS, &st, 1) != 0) {
            return SENS_E_BUS;
        }
        if ((st & ST_IM_UPDATE) == 0u) {
            break;
        }
        if (tries >= 5) {
            return SENS_E_BUSY;
        }
        bus->delay_ms(bus->ctx, 1);
    }

    uint8_t nvm[24];
    if (rd(bus, a, REG_CALIB, nvm, sizeof(nvm)) != 0) {
        return SENS_E_BUS;
    }
    bmp280_parse_calib(nvm, &dev->cal);
    if (dev->cal.t1 == 0u || dev->cal.p1 == 0u) {
        return SENS_E_STATUS;                       /* 보정값이 비어 있음 — 계산하면 엉뚱한 값 */
    }
    /* IIR 끔 · 대기 시간은 강제 모드라 안 씀 — 평균은 계산 모듈이 함. 다시 시작 뒤라 잠 상태 → 써짐 */
    return wr(bus, a, REG_CONFIG, 0x00u) == 0 ? SENS_OK : SENS_E_BUS;
}

sens_err_t bmp280_measure(const i2c_bus_t *bus, const bmp280_t *dev, bmp280_reading_t *out)
{
    const uint8_t a = dev->addr;
    if (a == 0u) {
        return SENS_E_BUS;
    }
    if (wr(bus, a, REG_CTRL_MEAS, BMP280_CTRL_MEAS) != 0) {
        return SENS_E_BUS;
    }
    bus->delay_ms(bus->ctx, BMP280_MEAS_MAX_MS);

    for (int tries = 0; ; tries++) {
        uint8_t st = 0;
        if (rd(bus, a, REG_STATUS, &st, 1) != 0) {
            return SENS_E_BUS;
        }
        if ((st & ST_MEASURING) == 0u) {
            break;
        }
        if (tries >= 5) {
            return SENS_E_BUSY;
        }
        bus->delay_ms(bus->ctx, 1);
    }

    uint8_t d[6];
    if (rd(bus, a, REG_DATA, d, sizeof(d)) != 0) {
        return SENS_E_BUS;
    }
    out->p_raw = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | ((uint32_t)d[2] >> 4));
    out->t_raw = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | ((uint32_t)d[5] >> 4));
    if (out->p_raw == RAW_SKIPPED || out->t_raw == RAW_SKIPPED) {
        return SENS_E_BUSY;                         /* 측정 안 된 칸 (강제 모드 명령이 안 먹음) */
    }
    return bmp280_compensate(&dev->cal, out->p_raw, out->t_raw, &out->p_pa, &out->t_c);
}
