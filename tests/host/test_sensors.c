/* 센서 드라이버 맥 시험 — 가짜 I2C 버스에 데이터시트대로 답하는 가짜 센서를 붙여 확인
 *   HSC: 데이터시트 Table 7 (10 % = 1638, 50 % = 8192, 90 % = 14746) · 온도 식
 *   ABP2: Table 6 (10 % = 1677722, 90 % = 15099494) · 온도 예제(6291456 → 25 °C) · 상태 바이트
 *   BMP390: Bosch BMP3_SensorAPI(db4cf8e) 실수 보정 코드로 낸 기준값과 대조
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "abp2.h"
#include "bmp390.h"
#include "hsc.h"
#include "i2c_timing.h"
#include "sensors.h"

static int fails = 0, checks = 0;

static void ok(const char *what, bool v)
{
    checks++;
    if (!v) {
        fails++;
    }
    printf("  %s %s\n", v ? "ok  " : "FAIL", what);
}

static void near(const char *what, double got, double want, double tol)
{
    checks++;
    const bool good = fabs(got - want) <= tol;
    if (!good) {
        fails++;
    }
    printf("  %s %-48s %14.6f  (기대 %.6f ± %g)\n", good ? "ok  " : "FAIL", what, got, want, tol);
}

/* ---------- 가짜 버스 ---------- */

typedef struct {
    /* HSC (I2C2, 0x28) */
    uint8_t hsc[4];
    bool hsc_dead;
    /* ABP2 (I2C3, 0x28) */
    uint8_t abp2[7];
    int abp2_busy_reads;        /* 명령 뒤 몇 번 busy 로 답할지 */
    int abp2_busy_left;
    uint8_t abp2_last_cmd[3];
    int abp2_cmds;
    /* BMP390 (I2C3, 0x76) */
    uint8_t bmp[128];
    bool bmp_dead;
    uint8_t bmp_status_after_measure;
    int bmp_pwr_writes;
    uint32_t delay_total;
} fake_t;

static int fk_write(void *ctx, uint8_t addr, const uint8_t *d, size_t n)
{
    fake_t *f = ctx;
    if (addr == HSC_ADDR && n == 3 && d[0] == 0xAA) {          /* ABP2 측정 명령 */
        memcpy(f->abp2_last_cmd, d, 3);
        f->abp2_cmds++;
        f->abp2_busy_left = f->abp2_busy_reads;
        return 0;
    }
    if (addr == BMP390_ADDR && !f->bmp_dead && n == 2) {
        f->bmp[d[0]] = d[1];
        if (d[0] == 0x1B) {
            f->bmp_pwr_writes++;
            f->bmp[0x03] = f->bmp_status_after_measure;
        }
        return 0;
    }
    return -1;
}

static int fk_read_dp(void *ctx, uint8_t addr, uint8_t *d, size_t n)
{
    fake_t *f = ctx;
    if (addr != HSC_ADDR || f->hsc_dead || n > 4) {
        return -1;
    }
    memcpy(d, f->hsc, n);
    return 0;
}

static int fk_read_pg(void *ctx, uint8_t addr, uint8_t *d, size_t n)
{
    fake_t *f = ctx;
    if (addr != ABP2_ADDR || n > 7) {
        return -1;
    }
    memcpy(d, f->abp2, n);
    if (f->abp2_busy_left > 0) {
        d[0] = 0x60;                /* 전원 · busy */
        f->abp2_busy_left--;
    }
    return 0;
}

static int fk_write_read(void *ctx, uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    fake_t *f = ctx;
    if (addr != BMP390_ADDR || f->bmp_dead || wn != 1 || w[0] + rn > sizeof(f->bmp)) {
        return -1;
    }
    memcpy(r, &f->bmp[w[0]], rn);
    return 0;
}

static void fk_delay(void *ctx, uint32_t ms) { ((fake_t *)ctx)->delay_total += ms; }

/* Bosch 기준값을 만든 보정값 세트 A · B (scratch 의 gold.c 와 같음) */
static const uint8_t CAL_A[21] = {0x67, 0x6B, 0xB2, 0x4A, 0xF9, 0x48, 0x21, 0xAC, 0x3F, 0x0A, 0xFE,
                                  0x20, 0x4E, 0x30, 0x75, 0x03, 0xFA, 0xDC, 0x05, 0x07, 0xC4};
static const uint8_t CAL_B[21] = {0x10, 0x80, 0x00, 0x40, 0x80, 0xFF, 0xFF, 0x00, 0x80, 0x81, 0x7F,
                                  0xFF, 0xFF, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x80, 0x80, 0x7F};

static void hsc_bytes(uint8_t b[4], uint8_t st, uint16_t p, uint16_t t)
{
    b[0] = (uint8_t)((st << 6) | (p >> 8));
    b[1] = (uint8_t)p;
    b[2] = (uint8_t)(t >> 3);
    b[3] = (uint8_t)((t & 7u) << 5);
}

static void abp2_bytes(uint8_t b[7], uint8_t st, uint32_t p, uint32_t t)
{
    b[0] = st;
    b[1] = (uint8_t)(p >> 16); b[2] = (uint8_t)(p >> 8); b[3] = (uint8_t)p;
    b[4] = (uint8_t)(t >> 16); b[5] = (uint8_t)(t >> 8); b[6] = (uint8_t)t;
}

static void bmp_raw(fake_t *f, uint32_t p, uint32_t t)
{
    f->bmp[0x04] = (uint8_t)p; f->bmp[0x05] = (uint8_t)(p >> 8); f->bmp[0x06] = (uint8_t)(p >> 16);
    f->bmp[0x07] = (uint8_t)t; f->bmp[0x08] = (uint8_t)(t >> 8); f->bmp[0x09] = (uint8_t)(t >> 16);
}

/* ---------- 시험 ---------- */

static void test_hsc(void)
{
    printf("HSC 차압 (±10 mbar = ±1000 Pa)\n");
    hsc_cfg_t c;
    hsc_default_cfg(&c);
    uint8_t b[4];
    hsc_reading_t r;
    hsc_bytes(b, 0, 8192, 768);
    ok("정상 상태 → OK", hsc_decode(b, &c, &r) == SENS_OK);
    near("50 % (8192) → 0 Pa", r.p_pa, 0.0, 1e-9);
    near("온도 768 → 25.03 °C", r.t_c, 768 * 200.0 / 2047.0 - 50.0, 1e-9);
    hsc_bytes(b, 0, 1638, 0);
    hsc_decode(b, &c, &r);
    near("10 % (1638) → −1000 Pa", r.p_pa, -1000.0, 1e-9);
    near("온도 0 → −50 °C", r.t_c, -50.0, 1e-9);
    hsc_bytes(b, 0, 14746, 2047);
    hsc_decode(b, &c, &r);
    near("90 % (14746) → +1000 Pa", r.p_pa, 1000.0, 1e-9);
    near("온도 2047 → 150 °C", r.t_c, 150.0, 1e-9);
    hsc_bytes(b, 0, 8192 + 7, 768);
    hsc_decode(b, &c, &r);
    near("7 눈금 = 1.068 Pa (한 눈금 0.153 Pa)", r.p_pa, 7 * 2000.0 / 13108.0, 1e-9);
    hsc_bytes(b, 2, 8192, 768);
    ok("상태 10 → 이전 값(STALE)", hsc_decode(b, &c, &r) == SENS_STALE);
    hsc_bytes(b, 3, 8192, 768);
    ok("상태 11 → 진단 오류", hsc_decode(b, &c, &r) == SENS_E_STATUS);
    hsc_bytes(b, 1, 8192, 768);
    ok("상태 01 → 명령 모드라 오류", hsc_decode(b, &c, &r) == SENS_E_STATUS);
    hsc_bytes(b, 0, 16000, 768);
    ok("90 % 넘음 → 범위 밖", hsc_decode(b, &c, &r) == SENS_E_RANGE);
}

static void test_abp2(void)
{
    printf("ABP2 정압 (0–6 kPa 게이지)\n");
    abp2_cfg_t c;
    abp2_default_cfg(&c);
    uint8_t b[7];
    abp2_reading_t r;
    abp2_bytes(b, 0x40, 1677722, 6291456);
    ok("전원 비트만 → OK", abp2_decode(b, &c, &r) == SENS_OK);
    near("10 % (1677722) → 0 Pa", r.p_pa, 0.0, 1e-9);
    near("온도 6291456 → 25 °C (데이터시트 예제, 반올림)", r.t_c, 25.0, 1e-5);
    abp2_bytes(b, 0x40, 15099494, 0);
    abp2_decode(b, &c, &r);
    near("90 % (15099494) → 6000 Pa", r.p_pa, 6000.0, 1e-9);
    abp2_bytes(b, 0x40, 8388608, 0);
    abp2_decode(b, &c, &r);
    near("50 % (8388608) → 3000 Pa", r.p_pa, 3000.0, 1e-3);
    abp2_bytes(b, 0x60, 8388608, 0);
    ok("busy → 아직 변환 중", abp2_decode(b, &c, &r) == SENS_E_BUSY);
    abp2_bytes(b, 0x44, 8388608, 0);
    ok("메모리 검사 실패 → 오류", abp2_decode(b, &c, &r) == SENS_E_STATUS);
    abp2_bytes(b, 0x41, 8388608, 0);
    ok("계산 포화 → 오류", abp2_decode(b, &c, &r) == SENS_E_STATUS);
    abp2_bytes(b, 0x00, 8388608, 0);
    ok("전원 비트 0 → 오류", abp2_decode(b, &c, &r) == SENS_E_STATUS);

    printf("ABP2 측정 순서 (0xAA 0x00 0x00 → 5 ms → 읽기, busy 면 다시)\n");
    fake_t f;
    memset(&f, 0, sizeof(f));
    abp2_bytes(f.abp2, 0x40, 1677722 + (15099494 - 1677722) / 20, 6291456);   /* 300 Pa */
    f.abp2_busy_reads = 2;
    i2c_bus_t bus = {fk_write, fk_read_pg, fk_write_read, fk_delay, &f};
    const sens_err_t e = abp2_measure(&bus, &c, &r);
    ok("busy 두 번 뒤 OK", e == SENS_OK);
    ok("명령 바이트 AA 00 00", f.abp2_cmds == 1 && f.abp2_last_cmd[0] == 0xAA && f.abp2_last_cmd[1] == 0 &&
                                     f.abp2_last_cmd[2] == 0);
    near("300 Pa", r.p_pa, 300.0, 0.001);
    ok("5 ms 기다린 뒤 1 ms 씩 두 번 더", f.delay_total == 7);
}

typedef struct {
    char set;
    uint32_t p_raw, t_raw;
    double p_pa, t_c;
    int clipped;
} gold_t;

/* Bosch BMP3_SensorAPI compensate_temperature · compensate_pressure (BMP3_FLOAT_COMPENSATION) 결과 */
static const gold_t GOLD[] = {
    {'A', 6600000u, 8400000u, 121621.883102, 24.196611, 0},
    {'A', 6800000u, 8200000u, 118426.640495, 20.647408, 0},
    {'A', 6400000u, 8700000u, 125000.000000, 29.516686, 1},
    {'A', 6600000u, 7038720u, 110266.592799, 0.000000, 0},
    {'A', 100u, 100u, 125000.000000, -40.000000, 1},
    {'A', 16777215u, 16777215u, 68316.145653, 85.000000, 1},
    {'A', 7400000u, 8400000u, 115494.987908, 24.196611, 0},
    {'A', 8200000u, 8400000u, 109329.471062, 24.196611, 0},
    {'A', 9000000u, 8400000u, 103120.336648, 24.196611, 0},
    {'A', 8600000u, 8100000u, 103727.171933, 18.872060, 0},
    {'B', 5000000u, 8000000u, 125000.000000, -6.062317, 1},
    {'B', 9000000u, 8500000u, 125000.000000, 1.631972, 1},
    {'B', 2000000u, 7000000u, 125000.000000, -22.133016, 1},
    {'B', 1000000u, 8300000u, 125000.000000, -1.418459, 1},
};

static void test_bmp390(void)
{
    printf("BMP390 보정식 — Bosch 공식 코드 기준값 %zu개와 대조\n", sizeof(GOLD) / sizeof(GOLD[0]));
    bmp390_calib_t ca, cb;
    bmp390_parse_calib(CAL_A, &ca);
    bmp390_parse_calib(CAL_B, &cb);
    double worst_p = 0, worst_t = 0;
    int flag_ok = 1;
    for (size_t i = 0; i < sizeof(GOLD) / sizeof(GOLD[0]); i++) {
        const gold_t *g = &GOLD[i];
        double p, t;
        const sens_err_t e = bmp390_compensate(g->set == 'A' ? &ca : &cb, g->p_raw, g->t_raw, &p, &t);
        worst_p = fmax(worst_p, fabs(p - g->p_pa));
        worst_t = fmax(worst_t, fabs(t - g->t_c));
        if ((e == SENS_E_RANGE) != (g->clipped != 0)) {
            flag_ok = 0;
        }
    }
    /* Bosch 코드는 거듭제곱을 float 로 계산해서 아주 작게 다를 수 있음 */
    near("기압 최대 차이 Pa", worst_p, 0.0, 0.05);
    near("온도 최대 차이 °C", worst_t, 0.0, 1e-5);
    ok("범위 밖 판정이 Bosch 와 같음", flag_ok);
    ok("변환 시간 ×4 · ×1 = 10,889 µs", bmp390_conv_time_us(2, 0) == 10889u);

    printf("BMP390 시작 · 측정 순서\n");
    fake_t f;
    memset(&f, 0, sizeof(f));
    f.bmp[0x00] = 0x60;
    memcpy(&f.bmp[0x31], CAL_A, 21);
    f.bmp_status_after_measure = 0x70;
    bmp_raw(&f, 9000000u, 8400000u);
    i2c_bus_t bus = {fk_write, fk_read_pg, fk_write_read, fk_delay, &f};
    bmp390_t dev;
    ok("칩 0x60 → 시작 OK", bmp390_init(&bus, &dev) == SENS_OK);
    ok("다시 시작 명령 0xB6", f.bmp[0x7E] == 0xB6);
    ok("OSR = 기압 ×4 · 온도 ×1 (0x02)", f.bmp[0x1C] == 0x02);
    ok("IIR 끔 (0x00)", f.bmp[0x1F] == 0x00);
    bmp390_reading_t r;
    ok("한 번 재기 OK", bmp390_measure(&bus, &dev, &r) == SENS_OK);
    ok("강제 모드 명령 0x13", f.bmp[0x1B] == 0x13);
    ok("원값을 아래 바이트 먼저로 읽음", r.p_raw == 9000000u && r.t_raw == 8400000u);
    near("기압 = Bosch 값", r.p_pa, 103120.336648, 0.05);
    near("온도 = Bosch 값", r.t_c, 24.196611, 1e-5);
    f.bmp[0x00] = 0x50;                 /* BMP388 */
    ok("다른 칩(0x50) → 칩 번호 오류", bmp390_init(&bus, &dev) == SENS_E_ID);
    f.bmp_dead = true;
    ok("응답 없음 → 버스 오류", bmp390_init(&bus, &dev) == SENS_E_BUS);
}

static bool power_state;
static void fake_power(bool on) { power_state = on; }

static void test_sensors_read(void)
{
    printf("세 센서 한 번에 읽기\n");
    fake_t f;
    memset(&f, 0, sizeof(f));
    hsc_bytes(f.hsc, 0, (uint16_t)(8192 + 32.4 * 13108.0 / 2000.0 + 0.5), 768);   /* 약 32.4 Pa */
    abp2_bytes(f.abp2, 0x40, 1677722 + (15099494 - 1677722) / 20, 6291456);       /* 300 Pa · 25 °C */
    f.bmp[0x00] = 0x60;
    memcpy(&f.bmp[0x31], CAL_A, 21);
    f.bmp_status_after_measure = 0x70;
    bmp_raw(&f, 9000000u, 8400000u);
    i2c_bus_t dp = {fk_write, fk_read_dp, fk_write_read, fk_delay, &f};
    i2c_bus_t pg = {fk_write, fk_read_pg, fk_write_read, fk_delay, &f};
    sensors_t s;
    sensors_init(&s, &dp, &pg, fake_power);
    ok("전원 켜고 BMP390 설정 OK", sensors_power_on(&s) == SENS_OK && power_state);
    sensors_result_t r;
    sensors_read(&s, &r);
    ok("셋 다 OK · 계산에 넣어도 됨", r.e_dp == SENS_OK && r.e_pg == SENS_OK && r.e_baro == SENS_OK && r.usable);
    near("차압 Pa", r.sample.dp_raw_pa, 32.4, 0.1);
    near("정압 Pa", r.sample.pg_pa, 300.0, 0.001);
    near("대기압 Pa (BMP390)", r.sample.patm_pa, 103120.336648, 0.05);
    near("가스 온도 °C (BMP390)", r.sample.t_gas_c, 24.196611, 1e-5);
    ok("기압을 쟀으니 '못 잼' 표시 없음", !r.sample.patm_held);

    printf("기압 센서가 죽으면 마지막 기압 · 정압 센서 온도로 대신\n");
    f.bmp_dead = true;
    sensors_read(&s, &r);
    ok("기압 오류로 남고 계산은 계속", r.e_baro == SENS_E_BUS && r.usable);
    near("마지막 기압 사용", r.sample.patm_pa, 103120.336648, 0.05);
    near("온도는 ABP2 것", r.sample.t_gas_c, 25.0, 1e-5);
    ok("'기압 못 잼' 표시", r.sample.patm_held);

    printf("범위를 살짝 벗어나면 버리지 않고 끝값으로\n");
    f.bmp_dead = false;
    hsc_bytes(f.hsc, 0, 14746 + 200, 768);                                         /* 90 % 넘음 → 1030 Pa 쯤 */
    abp2_bytes(f.abp2, 0x40, 1677722 - 3000, 6291456);                             /* 10 % 아래 → −1.3 Pa 쯤 */
    sensors_read(&s, &r);
    ok("둘 다 범위 밖 오류로 남지만 계산엔 넣음", r.e_dp == SENS_E_RANGE && r.e_pg == SENS_E_RANGE && r.usable && r.clipped);
    near("차압은 만점 1000 Pa 로", r.sample.dp_raw_pa, 1000.0, 1e-9);
    near("정압은 0 Pa 로", r.sample.pg_pa, 0.0, 1e-9);
    hsc_bytes(f.hsc, 0, (uint16_t)(8192 + 32.4 * 13108.0 / 2000.0 + 0.5), 768);
    abp2_bytes(f.abp2, 0x44, 8388608, 6291456);                                     /* 메모리 검사 실패 */
    sensors_read(&s, &r);
    ok("정압 센서 오류(범위 밖 아님)는 그대로 버림", r.e_pg == SENS_E_STATUS && !r.usable && !r.clipped);
    abp2_bytes(f.abp2, 0x40, 1677722 + (15099494 - 1677722) / 20, 6291456);

    printf("차압 센서가 죽으면 계산에 넣지 않음\n");
    f.hsc_dead = true;
    sensors_read(&s, &r);
    ok("usable = false", r.e_dp == SENS_E_BUS && !r.usable);

    printf("켤 때 기압 센서가 답이 없었으면 나중에 다시 설정해 봄 (11/18 검수)\n");
    f.hsc_dead = false;
    f.bmp_dead = true;
    sensors_t s2;
    sensors_init(&s2, &dp, &pg, fake_power);
    ok("켤 때 BMP390 응답 없음", sensors_power_on(&s2) == SENS_E_BUS && !s2.bmp_ready);
    sensors_read(&s2, &r);
    ok("해수면 값으로 계산하되 '못 잼' 표시", r.e_baro == SENS_E_BUS && r.usable && r.sample.patm_held &&
                                                r.sample.patm_pa == 101325.0);
    f.bmp_dead = false;
    int n = 1;
    while (n < 20 && r.e_baro != SENS_OK) {
        sensors_read(&s2, &r);
        n++;
    }
    ok("살아나면 6번째 읽기에 다시 설정 · 실제 기압", r.e_baro == SENS_OK && n == (int)SENSORS_BMP_RETRY &&
                                                         !r.sample.patm_held && s2.bmp_reinits == 1u);
    near("다시 설정 뒤 기압 = Bosch 값", r.sample.patm_pa, 103120.336648, 0.05);

    sensors_power_off(&s);
    ok("전원 끔", !power_state && !s.powered);
}

static void test_i2c_timing(void)
{
    printf("I2C 100 kHz 타이밍 값\n");
    ok("8 MHz → 0x10420F13 (RM0481 예와 같음)", i2c_timing_100k(8000000u) == 0x10420F13u);
    ok("32 MHz (리셋 직후) → 0x70420F13", i2c_timing_100k(32000000u) == 0x70420F13u);
    ok("64 MHz → 0xF0420F13", i2c_timing_100k(64000000u) == 0xF0420F13u);
    ok("30 MHz → 올려서 0x70420F13 (조금 느려짐)", i2c_timing_100k(30000000u) == 0x70420F13u);
}

int main(void)
{
    test_hsc();
    test_abp2();
    test_bmp390();
    test_sensors_read();
    test_i2c_timing();
    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
