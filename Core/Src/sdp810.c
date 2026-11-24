/* SDP810 차압 — 트리거 측정 · 잠 (sdp810.h 머리말) */
#include <string.h>
#include "sdp810.h"

static const uint8_t CMD_TRIGGER[2] = {0x36u, 0x2Fu};   /* 차압 온도 보상 · 클럭 늘림 없음 */
static const uint8_t CMD_SLEEP[2] = {0x36u, 0x77u};

uint8_t sdp810_crc8(const uint8_t *p)
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

void sdp810_init(sdp810_t *d)
{
    memset(d, 0, sizeof(*d));
    /* 처음엔 있다고 보고 한 번 찾아봄 (켤 때 잠/쉼 어느 쪽이든 trigger 가 처리) */
}

static void lost(sdp810_t *d)
{
    d->present = false;
    d->skip = SDP810_RETRY_READS;
    d->fails++;
}

sens_err_t sdp810_trigger(const i2c_bus_t *bus, sdp810_t *d)
{
    d->started = false;
    if (!d->present && d->skip > 0u) {
        d->skip--;
        return SENS_E_BUS;
    }
    if (bus->write(bus->ctx, SDP810_ADDR, CMD_TRIGGER, sizeof(CMD_TRIGGER)) != 0) {
        /* 잠들어 있으면 이 쓰기는 답이 없는 게 정상 — 이걸로 깸 */
        bus->delay_ms(bus->ctx, SDP810_WAKE_MS);
        if (bus->write(bus->ctx, SDP810_ADDR, CMD_TRIGGER, sizeof(CMD_TRIGGER)) != 0) {
            lost(d);
            return SENS_E_BUS;
        }
    }
    d->present = true;
    d->started = true;
    return SENS_OK;
}

sens_err_t sdp810_fetch(const i2c_bus_t *bus, sdp810_t *d, sdp810_reading_t *out)
{
    if (!d->started) {
        return SENS_E_BUS;
    }
    uint8_t b[9];
    if (bus->read(bus->ctx, SDP810_ADDR, b, sizeof(b)) != 0) {
        d->started = false;             /* 아직 측정 중이거나 빠짐 — 잠 명령도 못 받음 */
        lost(d);
        return SENS_E_BUS;
    }
    if (sdp810_crc8(&b[0]) != b[2] || sdp810_crc8(&b[3]) != b[5] || sdp810_crc8(&b[6]) != b[8]) {
        d->fails++;
        return SENS_E_STATUS;
    }
    const int16_t dp = (int16_t)((b[0] << 8) | b[1]);
    const int16_t t = (int16_t)((b[3] << 8) | b[4]);
    const int16_t scale = (int16_t)((b[6] << 8) | b[7]);
    if (scale <= 0) {
        d->fails++;
        return SENS_E_STATUS;
    }
    d->scale = scale;
    out->dp_pa = (double)dp / (double)scale;
    out->t_c = (double)t / 200.0;
    return SENS_OK;
}

void sdp810_sleep(const i2c_bus_t *bus, sdp810_t *d)
{
    if (!d->started) {
        return;
    }
    d->started = false;
    (void)bus->write(bus->ctx, SDP810_ADDR, CMD_SLEEP, sizeof(CMD_SLEEP));
}
