#include <stddef.h>
#include "lowpower.h"

uint32_t lp_tick_ms(bool flow_active)
{
    return flow_active ? LP_TICK_ACTIVE_MS : LP_TICK_IDLE_MS;
}

bool lp_wut_reload(uint32_t ms, uint32_t *reload)
{
    /* 깨우기 타이머는 (WUTR + 1) 틱 뒤에 울림 */
    const uint64_t ticks = ((uint64_t)ms * LP_WUT_HZ + 500u) / 1000u;
    if (ms == 0u || ticks < 1u || ticks > 65536u) {
        return false;
    }
    *reload = (uint32_t)(ticks - 1u);
    return true;
}

uint32_t lp_wut_actual_us(uint32_t reload)
{
    return (uint32_t)(((uint64_t)(reload + 1u) * 1000000u + LP_WUT_HZ / 2u) / LP_WUT_HZ);
}

bool lp_lpuart_brr(uint32_t kernel_hz, uint32_t baud, uint32_t *brr, int32_t *err_ppm)
{
    if (baud == 0u || kernel_hz < 3u * baud || kernel_hz > 4096u * baud) {
        return false;
    }
    const uint64_t b = ((uint64_t)kernel_hz * 256u + baud / 2u) / baud;
    if (b < 0x300u || b > 0xFFFFFu) {
        return false;
    }
    *brr = (uint32_t)b;
    if (err_ppm != NULL) {
        const double actual = (double)kernel_hz * 256.0 / (double)b;
        *err_ppm = (int32_t)((actual - (double)baud) / (double)baud * 1e6);
    }
    return true;
}

/* 날짜 ↔ 일수: Howard Hinnant 의 days_from_civil (공개 알고리즘) */
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2u;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153u * (m + (m > 2u ? (uint32_t)-3 : 9u)) + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

uint32_t lp_unix_from_civil(const lp_civil_t *c)
{
    const int32_t days = days_from_civil(c->year, c->month, c->day);
    return (uint32_t)days * 86400u + c->hour * 3600u + c->min * 60u + c->sec;
}

void lp_civil_from_unix(uint32_t t, lp_civil_t *c)
{
    const int32_t z = (int32_t)(t / 86400u) + 719468;
    uint32_t s = t % 86400u;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int32_t y = (int32_t)yoe + era * 400;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    const uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    const uint32_t m = mp < 10u ? mp + 3u : mp - 9u;
    c->year = (uint16_t)(y + (m <= 2u));
    c->month = (uint8_t)m;
    c->day = (uint8_t)d;
    c->hour = (uint8_t)(s / 3600u);
    s %= 3600u;
    c->min = (uint8_t)(s / 60u);
    c->sec = (uint8_t)(s % 60u);
}

bool lp_minute_due(uint32_t last_minute, uint32_t now_s)
{
    return now_s / 60u != last_minute;
}
