#include <string.h>
#include "persist.h"

static void put_f64(uint8_t *p, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(u >> (8 * i));
    }
}

static double get_f64(const uint8_t *p)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= (uint64_t)p[i] << (8 * i);
    }
    double v;
    memcpy(&v, &u, 8);
    return v;
}

static void put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

void settings_default(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->band = 7;    /* 국내 시험 KR920 */
}

size_t settings_pack(const settings_t *s, uint8_t *o)
{
    put_f64(o + 0, s->zero_pa);
    put_f64(o + 8, s->vent_p_pa);
    o[16] = s->band;
    o[17] = (uint8_t)((s->zero_set ? 1u : 0u) | (s->keys_set ? 2u : 0u));
    memcpy(o + 18, s->dev_eui, 8);
    memcpy(o + 26, s->app_eui, 8);
    memcpy(o + 34, s->app_key, 16);
    return PERSIST_SETTINGS_LEN;
}

void settings_unpack(const uint8_t *in, settings_t *s)
{
    s->zero_pa = get_f64(in + 0);
    s->vent_p_pa = get_f64(in + 8);
    s->band = in[16];
    s->zero_set = (in[17] & 1u) != 0u;
    s->keys_set = (in[17] & 2u) != 0u;
    memcpy(s->dev_eui, in + 18, 8);
    memcpy(s->app_eui, in + 26, 8);
    memcpy(s->app_key, in + 34, 16);
}

size_t counter_pack(const counter_t *c, uint8_t *o)
{
    put_f64(o + 0, c->total_nm3);
    put_f64(o + 8, c->total_m3);
    put_u32(o + 16, c->ring_next_seq);
    o[20] = (uint8_t)c->report_seq;
    o[21] = (uint8_t)(c->report_seq >> 8);
    put_u32(o + 22, c->vent_minutes);
    put_u32(o + 26, c->depletions);
    put_u32(o + 30, c->events);
    put_u32(o + 34, c->unix_s);
    return PERSIST_COUNTER_LEN;
}

void counter_unpack(const uint8_t *in, counter_t *c)
{
    c->total_nm3 = get_f64(in + 0);
    c->total_m3 = get_f64(in + 8);
    c->ring_next_seq = get_u32(in + 16);
    c->report_seq = (uint16_t)(in[20] | in[21] << 8);
    c->vent_minutes = get_u32(in + 22);
    c->depletions = get_u32(in + 26);
    c->events = get_u32(in + 30);
    c->unix_s = get_u32(in + 34);
}

nvlog_err_t settings_load(const nvlog_t *l, settings_t *s)
{
    uint8_t b[PERSIST_SETTINGS_LEN];
    const nvlog_err_t e = nvlog_load(l, b, sizeof(b), NULL);
    if (e != NVLOG_OK) {
        settings_default(s);
        return e;
    }
    settings_unpack(b, s);
    return NVLOG_OK;
}

nvlog_err_t settings_save(nvlog_t *l, const settings_t *s)
{
    uint8_t b[PERSIST_SETTINGS_LEN];
    return nvlog_save(l, b, settings_pack(s, b));
}

nvlog_err_t counter_load(const nvlog_t *l, counter_t *c)
{
    uint8_t b[PERSIST_COUNTER_LEN];
    const nvlog_err_t e = nvlog_load(l, b, sizeof(b), NULL);
    if (e != NVLOG_OK) {
        memset(c, 0, sizeof(*c));
        return e;
    }
    counter_unpack(b, c);
    return NVLOG_OK;
}

nvlog_err_t counter_save(nvlog_t *l, const counter_t *c)
{
    uint8_t b[PERSIST_COUNTER_LEN];
    return nvlog_save(l, b, counter_pack(c, b));
}
