/* 센서 드라이버 맥 시험 — 가짜 I2C 버스에 데이터시트대로 답하는 가짜 센서를 붙여 확인
 *   HSC: 데이터시트 Table 7 (10 % = 1638, 50 % = 8192, 90 % = 14746) · 온도 식
 *   ABP2: Table 6 (10 % = 1677722, 90 % = 15099494) · 온도 예제(6291456 → 25 °C) · 상태 바이트
 *   BMP280: Bosch BME280_SensorAPI(c90d419) 실수 보정 코드(온도 · 기압 부분 = BMP280 데이터시트 8.1)로 낸 기준값과 대조
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "abp2.h"
#include "ds18b20.h"
#include "bmp280.h"
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
    /* BMP280 (I2C3, 0x76 — bmp_addr 로 바꿈) */
    uint8_t bmp[256];
    uint8_t bmp_addr;               /* 0 이면 0x76 */
    bool bmp_dead;
    uint8_t bmp_status_after_measure;   /* 강제 모드 명령 뒤 0xF3 값 (0 = 끝남) */
    int bmp_ctrl_writes, bmp_resets;
    uint32_t delay_total;
} fake_t;

static bool bmp_here(const fake_t *f, uint8_t addr)
{
    return !f->bmp_dead && addr == (f->bmp_addr != 0u ? f->bmp_addr : BMP280_ADDR_LO);
}

static int fk_write(void *ctx, uint8_t addr, const uint8_t *d, size_t n)
{
    fake_t *f = ctx;
    if (addr == HSC_ADDR && n == 3 && d[0] == 0xAA) {          /* ABP2 측정 명령 */
        memcpy(f->abp2_last_cmd, d, 3);
        f->abp2_cmds++;
        f->abp2_busy_left = f->abp2_busy_reads;
        return 0;
    }
    if (bmp_here(f, addr) && n == 2) {
        f->bmp[d[0]] = d[1];
        if (d[0] == 0xF4) {
            f->bmp_ctrl_writes++;
            f->bmp[0xF3] = f->bmp_status_after_measure;
        }
        if (d[0] == 0xE0 && d[1] == 0xB6) {
            f->bmp_resets++;
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
    if (!bmp_here(f, addr) || wn != 1 || w[0] + rn > sizeof(f->bmp)) {
        return -1;
    }
    memcpy(r, &f->bmp[w[0]], rn);
    return 0;
}

static void fk_delay(void *ctx, uint32_t ms) { ((fake_t *)ctx)->delay_total += ms; }

/* Bosch 기준값을 만든 보정값 세트 A · B (scratch bmp280/gen/gold.c 와 같음)
 * A = 널리 쓰이는 BMP280 예제 값 dig_T1 27504 · T2 26435 · T3 −1000 · P1 36477 · P2 −10685 · P3 3024 · P4 2855 ·
 *     P5 140 · P6 −7 · P7 15500 · P8 −14600 · P9 6000 (원값 415148 · 519888 → 100,653.26 Pa · 25.08 °C)
 * B = 다른 칩처럼 바꾼 세트 */
static const uint8_t CAL_A[24] = {0x70, 0x6B, 0x43, 0x67, 0x18, 0xFC, 0x7D, 0x8E, 0x43, 0xD6, 0xD0, 0x0B,
                                  0x27, 0x0B, 0x8C, 0x00, 0xF9, 0xFF, 0x8C, 0x3C, 0xF8, 0xC6, 0x70, 0x17};
static const uint8_t CAL_B[24] = {0x69, 0x6D, 0xF4, 0x65, 0x32, 0x00, 0x7C, 0x92, 0x98, 0xD6, 0xD0, 0x0B,
                                  0x94, 0x11, 0x6A, 0xFF, 0xF9, 0xFF, 0xAC, 0x26, 0x0A, 0xD8, 0xBD, 0x10};
#define BMP_P_PA  100653.258145     /* 세트 A · 원값 415148 · 519888 의 Bosch 값 */
#define BMP_T_C   25.082478

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

/* 0xF7–0xFC: 20비트 원값, 위 바이트 먼저 · xlsb 는 [7:4] */
static void bmp_raw(fake_t *f, uint32_t p, uint32_t t)
{
    f->bmp[0xF7] = (uint8_t)(p >> 12); f->bmp[0xF8] = (uint8_t)(p >> 4); f->bmp[0xF9] = (uint8_t)((p & 0xFu) << 4);
    f->bmp[0xFA] = (uint8_t)(t >> 12); f->bmp[0xFB] = (uint8_t)(t >> 4); f->bmp[0xFC] = (uint8_t)((t & 0xFu) << 4);
}

/* 칩 0x58 · 세트 A · 원값 415148 · 519888 */
static void bmp_setup(fake_t *f)
{
    f->bmp[0xD0] = 0x58;
    memcpy(&f->bmp[0x88], CAL_A, 24);
    f->bmp_status_after_measure = 0x00;
    bmp_raw(f, 415148u, 519888u);
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
    int32_t p_raw, t_raw;
    double p_pa, t_c;
    int clipped;
} gold_t;

/* Bosch BME280_SensorAPI(c90d419) bme280_compensate_data(BME280_PRESS | BME280_TEMP) 실수 결과 */
static const gold_t GOLD[] = {
    {'A', 415148, 519888, 100653.258145, 25.082478, 0},
    {'A', 330000, 519888, 110000.000000, 25.082478, 1},
    {'A', 380000, 500000, 105706.073695, 18.846777, 0},
    {'A', 450000, 540000, 95552.131529, 31.379266, 0},
    {'A', 300000, 480000, 110000.000000, 12.566890, 1},
    {'A', 560000, 519888, 75756.632349, 25.082478, 0},
    {'A', 100000, 519888, 110000.000000, 25.082478, 1},
    {'A', 415148, 0, 77126.364219, -40.000000, 1},
    {'A', 415148, 1048575, 110000.000000, 85.000000, 1},
    {'A', 1048575, 519888, 30000.000000, 25.082478, 1},
    {'B', 415148, 519888, 93038.776769, 22.325085, 0},
    {'B', 350000, 505000, 103103.031763, 17.691800, 0},
    {'B', 470000, 530000, 84356.411988, 25.472177, 0},
    {'B', 520000, 460000, 73250.599189, 3.688911, 0},
};

static void test_bmp280(void)
{
    printf("BMP280 보정식 — Bosch 공식 코드 기준값 %zu개와 대조\n", sizeof(GOLD) / sizeof(GOLD[0]));
    bmp280_calib_t ca, cb;
    bmp280_parse_calib(CAL_A, &ca);
    bmp280_parse_calib(CAL_B, &cb);
    ok("보정값 읽기: T1 27504 · T3 −1000 · P1 36477 · P6 −7 · P9 6000 (표 17, 아래 바이트 먼저)",
       ca.t1 == 27504u && ca.t2 == 26435 && ca.t3 == -1000 && ca.p1 == 36477u && ca.p2 == -10685 && ca.p6 == -7 &&
       ca.p8 == -14600 && ca.p9 == 6000);
    double worst_p = 0, worst_t = 0;
    int flag_ok = 1;
    for (size_t i = 0; i < sizeof(GOLD) / sizeof(GOLD[0]); i++) {
        const gold_t *g = &GOLD[i];
        double p, t;
        const sens_err_t e = bmp280_compensate(g->set == 'A' ? &ca : &cb, g->p_raw, g->t_raw, &p, &t);
        worst_p = fmax(worst_p, fabs(p - g->p_pa));
        worst_t = fmax(worst_t, fabs(t - g->t_c));
        if ((e == SENS_E_RANGE) != (g->clipped != 0)) {
            flag_ok = 0;
        }
    }
    near("기압 최대 차이 Pa", worst_p, 0.0, 1e-5);
    near("온도 최대 차이 °C", worst_t, 0.0, 1e-6);
    ok("범위 밖 판정(−40…85 °C · 300…1100 hPa)이 Bosch 와 같음", flag_ok);
    bmp280_calib_t cz = ca;
    cz.p1 = 0;
    double pz, tz;
    ok("P1 = 0 → 0 으로 안 나누고 끝값 · 범위 밖", bmp280_compensate(&cz, 415148, 519888, &pz, &tz) == SENS_E_RANGE &&
                                                      pz == 30000.0);

    printf("BMP280 시작 · 측정 순서\n");
    fake_t f;
    memset(&f, 0, sizeof(f));
    bmp_setup(&f);
    f.bmp[0xF5] = 0xFF;
    i2c_bus_t bus = {fk_write, fk_read_pg, fk_write_read, fk_delay, &f};
    bmp280_t dev;
    ok("0x76 칩 0x58 → 시작 OK", bmp280_init(&bus, &dev) == SENS_OK && dev.addr == 0x76 && dev.chip_id == 0x58);
    ok("다시 시작 명령 0xE0 ← 0xB6 한 번", f.bmp_resets == 1);
    ok("IIR 끔 (0xF5 = 0x00)", f.bmp[0xF5] == 0x00);
    ok("보정값 읽음", dev.cal.t1 == 27504u && dev.cal.p9 == 6000);
    bmp280_reading_t r;
    uint32_t d0 = f.delay_total;
    ok("한 번 재기 OK", bmp280_measure(&bus, &dev, &r) == SENS_OK);
    ok("강제 모드 명령 0xF4 = 0x2D (온도 ×1 · 기압 ×4 · 강제)", f.bmp[0xF4] == 0x2D && f.bmp_ctrl_writes == 1);
    ok("최대 측정 시간 14 ms 를 기다림 (표 13: 13.3 ms)", f.delay_total - d0 == BMP280_MEAS_MAX_MS);
    ok("20비트 원값을 위 바이트 먼저로 읽음", r.p_raw == 415148 && r.t_raw == 519888);
    near("기압 = Bosch 값", r.p_pa, BMP_P_PA, 1e-5);
    near("온도 = Bosch 값", r.t_c, BMP_T_C, 1e-6);

    f.bmp_status_after_measure = 0x08;                  /* measuring 이 안 내려감 */
    ok("측정이 안 끝나면 busy", bmp280_measure(&bus, &dev, &r) == SENS_E_BUSY);
    f.bmp_status_after_measure = 0x00;
    bmp_raw(&f, 0x80000u, 519888u);
    ok("건너뛴 칸(0x80000) → busy", bmp280_measure(&bus, &dev, &r) == SENS_E_BUSY);
    bmp_raw(&f, 415148u, 519888u);
    f.bmp[0xF3] = 0x01;                                 /* 보정값 옮기는 중이 안 끝남 */
    f.bmp_status_after_measure = 0x01;
    f.bmp_ctrl_writes = 0;
    ok("다시 시작 뒤 im_update 가 안 내려가면 busy", bmp280_init(&bus, &dev) == SENS_E_BUSY);
    f.bmp[0xF3] = 0x00;
    f.bmp_status_after_measure = 0x00;

    f.bmp_addr = 0x77;
    ok("0x77 모듈(SDO = 3V3)도 찾음", bmp280_init(&bus, &dev) == SENS_OK && dev.addr == 0x77);
    ok("0x77 에서 재기 OK", bmp280_measure(&bus, &dev, &r) == SENS_OK && fabs(r.p_pa - BMP_P_PA) < 1e-5);
    f.bmp_addr = 0;
    f.bmp[0xD0] = 0x60;
    ok("BME280 모듈(칩 0x60)도 받음", bmp280_init(&bus, &dev) == SENS_OK && dev.chip_id == 0x60);
    f.bmp[0xD0] = 0x56;
    ok("BMP280 시제품 칩(0x56)도 받음", bmp280_init(&bus, &dev) == SENS_OK);
    f.bmp[0xD0] = 0x00;                                 /* BMP280 은 0xD0 이 칩 번호가 아님 */
    ok("다른 칩 → 칩 번호 오류 · 주소 없음", bmp280_init(&bus, &dev) == SENS_E_ID && dev.addr == 0u);
    ok("주소 없이 재기 → 버스 오류 (버스 안 건드림)", bmp280_measure(&bus, &dev, &r) == SENS_E_BUS);
    f.bmp[0xD0] = 0x58;
    memset(&f.bmp[0x88], 0, 24);
    ok("보정값이 0 → 상태 오류", bmp280_init(&bus, &dev) == SENS_E_STATUS);
    memcpy(&f.bmp[0x88], CAL_A, 24);
    f.bmp_dead = true;
    ok("응답 없음(두 주소 다) → 버스 오류", bmp280_init(&bus, &dev) == SENS_E_BUS);
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
    bmp_setup(&f);
    i2c_bus_t dp = {fk_write, fk_read_dp, fk_write_read, fk_delay, &f};
    i2c_bus_t pg = {fk_write, fk_read_pg, fk_write_read, fk_delay, &f};
    sensors_t s;
    sensors_init(&s, &dp, &pg, fake_power);
    ok("전원 켜고 BMP280 설정 OK", sensors_power_on(&s) == SENS_OK && power_state);
    sensors_result_t r;
    sensors_read(&s, &r);
    ok("셋 다 OK · 계산에 넣어도 됨", r.e_dp == SENS_OK && r.e_pg == SENS_OK && r.e_baro == SENS_OK && r.usable);
    near("차압 Pa", r.sample.dp_raw_pa, 32.4, 0.1);
    near("정압 Pa", r.sample.pg_pa, 300.0, 0.001);
    near("대기압 Pa (BMP280)", r.sample.patm_pa, BMP_P_PA, 0.05);
    near("가스 온도 °C (BMP280)", r.sample.t_gas_c, BMP_T_C, 1e-5);
    ok("기압을 쟀으니 '못 잼' 표시 없음", !r.sample.patm_held);

    printf("기압 센서가 죽으면 마지막 기압 · 정압 센서 온도로 대신\n");
    f.bmp_dead = true;
    sensors_read(&s, &r);
    ok("기압 오류로 남고 계산은 계속", r.e_baro == SENS_E_BUS && r.usable);
    near("마지막 기압 사용", r.sample.patm_pa, BMP_P_PA, 0.05);
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
    ok("켤 때 BMP280 응답 없음", sensors_power_on(&s2) == SENS_E_BUS && !s2.bmp_ready);
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
    near("다시 설정 뒤 기압 = Bosch 값", r.sample.patm_pa, BMP_P_PA, 0.05);

    sensors_power_off(&s);
    ok("전원 끔", !power_state && !s.powered);
}


/* ---------- 다음 판 센서: SDP810 (I2C3 0x25) · DS18B20 (1-Wire) ---------- */

typedef struct {
    fake_t f;                   /* 맨 앞 — fk_* 함수가 그대로 씀 */
    /* 가짜 SDP810 (데이터시트 6.3.3 · 6.3.5) */
    bool sdp_present, sdp_asleep, sdp_measuring, sdp_corrupt, sdp_read_fail;
    uint32_t sdp_t_start, sdp_wake_t;
    int16_t sdp_raw, sdp_temp, sdp_scale;
    int sdp_triggers, sdp_wake_nacks, sdp_sleeps, sdp_early_reads, sdp_traffic;
    /* 버스 실패 이어짐 (i2c_stm32.c: 3번 이어지면 버스를 다시 엶) */
    int consec, max_consec;
} pgx_t;

static pgx_t *g_px;
static uint32_t px_tick(void) { return g_px->f.delay_total; }
static uint32_t g_wall;
static uint32_t px_wall(void) { return g_wall; }

static int px_note(pgx_t *x, int rc)
{
    x->consec = rc != 0 ? x->consec + 1 : 0;
    if (x->consec > x->max_consec) {
        x->max_consec = x->consec;
    }
    return rc;
}

static uint8_t crc_sens(uint8_t a, uint8_t b)
{
    const uint8_t two[2] = {a, b};
    return sdp810_crc8(two);
}

static int px_write(void *ctx, uint8_t addr, const uint8_t *d, size_t n)
{
    pgx_t *x = ctx;
    if (addr != SDP810_ADDR) {
        return px_note(x, fk_write(ctx, addr, d, n));
    }
    x->sdp_traffic++;
    const uint32_t now = x->f.delay_total;
    if (!x->sdp_present) {
        return px_note(x, -1);
    }
    if (x->sdp_asleep) {                         /* 주소 + 쓰기로 깸, 그 쓰기엔 답 없음 */
        x->sdp_asleep = false;
        x->sdp_wake_t = now;
        x->sdp_wake_nacks++;
        return px_note(x, -1);
    }
    if (now - x->sdp_wake_t < 2u) {
        return px_note(x, -1);                   /* 아직 깨는 중 */
    }
    if (x->sdp_measuring && now - x->sdp_t_start < 45u) {
        return px_note(x, -1);                   /* 측정 중엔 명령 못 받음 */
    }
    if (n == 2 && d[0] == 0x36 && d[1] == 0x2F) {
        x->sdp_measuring = true;
        x->sdp_t_start = now;
        x->sdp_triggers++;
        return px_note(x, 0);
    }
    if (n == 2 && d[0] == 0x36 && d[1] == 0x77) {
        x->sdp_asleep = true;
        x->sdp_sleeps++;
        return px_note(x, 0);
    }
    return px_note(x, -1);
}

static int px_read(void *ctx, uint8_t addr, uint8_t *d, size_t n)
{
    pgx_t *x = ctx;
    if (addr != SDP810_ADDR) {
        return px_note(x, fk_read_pg(ctx, addr, d, n));
    }
    x->sdp_traffic++;
    if (!x->sdp_present || x->sdp_asleep || !x->sdp_measuring || n != 9) {
        return px_note(x, -1);
    }
    if (x->sdp_read_fail) {                      /* 선 잡음 등으로 읽기만 한 번 실패 — 센서는 측정 끝 · 쉼 상태 */
        x->sdp_measuring = false;
        return px_note(x, -1);
    }
    if (x->f.delay_total - x->sdp_t_start < 45u) {
        x->sdp_early_reads++;
        return px_note(x, -1);                   /* 아직 결과 없음 → 읽기 머리에 답 안 함 */
    }
    const int16_t v[3] = {x->sdp_raw, x->sdp_temp, x->sdp_scale};
    for (int i = 0; i < 3; i++) {
        d[i * 3] = (uint8_t)((uint16_t)v[i] >> 8);
        d[i * 3 + 1] = (uint8_t)v[i];
        d[i * 3 + 2] = crc_sens(d[i * 3], d[i * 3 + 1]);
    }
    if (x->sdp_corrupt) {
        d[2] ^= 0x01u;
    }
    x->sdp_measuring = false;                    /* 측정 끝 · 쉼 상태 */
    return px_note(x, 0);
}

static int px_write_read(void *ctx, uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    return px_note(ctx, fk_write_read(ctx, addr, w, wn, r, rn));
}

/* 가짜 DS18B20 — 비트 단위로 명령을 받아 스크래치패드를 비트로 내보냄 (작은 비트 먼저) */
typedef struct {
    bool present;
    uint8_t pad[9];
    uint8_t cur;
    int nbits;
    uint8_t last_cmd;
    bool reading;
    int ridx;
    int converts, resets;
} fow_t;

static bool fow_reset(void *ctx)
{
    fow_t *o = ctx;
    o->resets++;
    o->nbits = 0;
    o->cur = 0;
    o->reading = false;
    o->last_cmd = 0;
    return o->present;
}

static void fow_write_bit(void *ctx, bool bit)
{
    fow_t *o = ctx;
    if (bit) {
        o->cur |= (uint8_t)(1u << o->nbits);
    }
    if (++o->nbits == 8) {
        if (o->last_cmd == 0xCC && o->cur == 0x44) {
            o->converts++;
        }
        if (o->last_cmd == 0xCC && o->cur == 0xBE) {
            o->reading = true;
            o->ridx = 0;
        }
        o->last_cmd = o->cur;
        o->cur = 0;
        o->nbits = 0;
    }
}

static bool fow_read_bit(void *ctx)
{
    fow_t *o = ctx;
    if (!o->present || !o->reading || o->ridx >= 72) {
        return true;                             /* 아무도 안 당기면 풀업으로 1 */
    }
    const bool b = (o->pad[o->ridx / 8] >> (o->ridx % 8)) & 1u;
    o->ridx++;
    return b;
}

static void fow_temp(fow_t *o, int16_t raw)
{
    const uint8_t p[8] = {(uint8_t)raw, (uint8_t)((uint16_t)raw >> 8), 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10};
    memcpy(o->pad, p, 8);
    o->pad[8] = onewire_crc8(o->pad, 8);
}

static void test_ds18b20(void)
{
    printf("DS18B20 (1-Wire, 데이터시트 Dallas 050400)\n");
    const uint8_t rom[7] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00};
    ok("CRC-8 = A2h (Maxim 설명서 예: 02 1C B8 01 00 00 00)", onewire_crc8(rom, 7) == 0xA2u);
    fow_t o;
    memset(&o, 0, sizeof(o));
    o.present = true;
    const onewire_t ow = {fow_reset, fow_write_bit, fow_read_bit, &o};
    ok("변환 시작: 존재 펄스 · CC 44 받음", ds18b20_convert(&ow) == SENS_OK && o.converts == 1);
    fow_temp(&o, 0x0157);                        /* 343 ÷ 16 = 21.4375 °C */
    double t = 0.0;
    ok("읽기 OK", ds18b20_read(&ow, &t) == SENS_OK);
    near("21.4375 °C (0x0157 ÷ 16)", t, 21.4375, 1e-9);
    fow_temp(&o, (int16_t)0xFF5E);               /* 데이터시트 표: −10.125 °C */
    ok("영하 −10.125 °C (FF5Eh)", ds18b20_read(&ow, &t) == SENS_OK && t == -10.125);
    fow_temp(&o, 0x0550);
    ok("85.0 °C (켠 뒤 기본값) → 아직 안 잰 값", ds18b20_read(&ow, &t) == SENS_STALE);
    fow_temp(&o, 0x0157);
    o.pad[8] ^= 0x10u;
    ok("CRC 틀림 → 오류", ds18b20_read(&ow, &t) == SENS_E_STATUS);
    memset(o.pad, 0, sizeof(o.pad));
    ok("전부 0(선이 낮게 붙음, CRC 도 0) → 오류", ds18b20_read(&ow, &t) == SENS_E_STATUS);
    o.present = false;
    ok("존재 펄스 없음 → 없음(버스 오류)", ds18b20_convert(&ow) == SENS_E_BUS && ds18b20_read(&ow, &t) == SENS_E_BUS);
}

static void test_next_board_sensors(void)
{
    printf("다음 판 센서 묶음: SDP810 깨우기 · 50 ms · 잠, DS18B20 60 초마다, 없으면 같은 버스 센서에 영향 없음\n");
    pgx_t x;
    memset(&x, 0, sizeof(x));
    g_px = &x;
    fake_t *f = &x.f;
    hsc_bytes(f->hsc, 0, (uint16_t)(8192 + 32.4 * 13108.0 / 2000.0 + 0.5), 768);
    abp2_bytes(f->abp2, 0x40, 1677722 + (15099494 - 1677722) / 20, 6291456);
    bmp_setup(f);
    x.sdp_present = true;
    x.sdp_asleep = true;                         /* 지난 읽기 뒤 잠든 상태로 시작 */
    x.sdp_raw = 1912;                            /* 1912 ÷ 60 = 31.87 Pa */
    x.sdp_temp = 4500;                           /* ÷ 200 = 22.5 °C */
    x.sdp_scale = 60;
    fow_t o;
    memset(&o, 0, sizeof(o));
    o.present = true;
    fow_temp(&o, 0x0157);
    const onewire_t ow = {fow_reset, fow_write_bit, fow_read_bit, &o};
    i2c_bus_t dp = {fk_write, fk_read_dp, fk_write_read, fk_delay, f};
    i2c_bus_t pg = {px_write, px_read, px_write_read, fk_delay, &x};
    sensors_t s;
    sensors_init(&s, &dp, &pg, fake_power);
    sensors_set_extra(&s, &ow, px_tick, px_wall);
    sensors_power_on(&s);
    sensors_result_t r;

    g_wall = 0;
    uint32_t d0 = f->delay_total;
    sensors_read(&s, &r);
    ok("세 센서 그대로 OK", r.e_dp == SENS_OK && r.e_pg == SENS_OK && r.e_baro == SENS_OK && r.usable);
    ok("SDP810: 깨우기 답 없음 1번 → 다시 시작 → 읽음", r.e_sdp == SENS_OK && r.sample.dp2_ok && x.sdp_wake_nacks == 1 &&
                                                            x.sdp_triggers == 1);
    near("SDP810 차압 Pa (1912 ÷ 60, 대기압 보정 × 96,600 ÷ (대기압 + 정압))", r.sample.dp2_pa,
         31.866667 * 96600.0 / (r.sample.patm_pa + r.sample.pg_pa), 1e-5);
    ok("보정 전 값과 다름 (이 시험 기압이 966 hPa 가 아님)", fabs(r.sample.dp2_pa - 31.866667) > 0.01);
    near("보정 식: Kisii 82,500 Pa 에서 10 Pa → 11.7091 Pa", sdp810_p_comp(10.0, 82500.0), 11.709090909, 1e-8);
    near("보정 식: 966 hPa 에서는 그대로", sdp810_p_comp(10.0, 96600.0), 10.0, 1e-12);
    near("보정 식: 벤치 102,040 Pa 에서 −5 Pa → −4.7334 Pa", sdp810_p_comp(-5.0, 102040.0), -4.733437868, 1e-8);
    near("보정 식: 절대압이 30 kPa 이하(이상한 값)면 그대로", sdp810_p_comp(10.0, 20000.0), 10.0, 1e-12);
    near("SDP810 칩 온도 °C (4500 ÷ 200)", r.t_sdp_c, 22.5, 1e-9);
    ok("측정 중엔 안 물음 (이른 읽기 0)", x.sdp_early_reads == 0);
    ok("읽은 뒤 다시 잠 (잠 명령 1번)", x.sdp_asleep && x.sdp_sleeps == 1);
    ok("버스 실패가 이어져도 1번 (다시 열기 문턱 3 안)", x.max_consec <= 1);
    ok("읽기 한 번 시간 ≤ 60 ms (SDP 50 ms 를 다른 센서와 겹쳐 씀)", f->delay_total - d0 <= 60u);
    ok("DS18B20: 처음엔 변환만 시작 · 가스 온도는 BMP280", r.e_probe == SENS_E_BUS && !r.sample.t_probe_ok &&
                                                                o.converts == 1 && fabs(r.sample.t_gas_c - BMP_T_C) < 1e-5);

    g_wall = 1000;
    sensors_read(&s, &r);
    ok("1 초 뒤: DS18B20 읽음 · 가스 온도 = 프로브", r.e_probe == SENS_OK && r.sample.t_probe_ok &&
                                                       r.sample.t_gas_c == 21.4375 && r.sample.t_probe_c == 21.4375);
    near("함체 안 온도(BMP280)는 따로 남음", r.t_board_c, BMP_T_C, 1e-5);
    ok("SDP810 두 번째도 OK (깨우기 2번째)", r.e_sdp == SENS_OK && x.sdp_wake_nacks == 2 && x.max_consec <= 1);

    g_wall = 2000;
    sensors_read(&s, &r);
    ok("2 초: 새 변환 안 함(60 초 간격) · 최근 값 그대로", o.converts == 1 && r.e_probe == SENS_STALE && r.sample.t_probe_ok);
    g_wall = 59000;
    sensors_read(&s, &r);
    ok("59 초: 아직 변환 안 함", o.converts == 1);
    g_wall = 60000;
    sensors_read(&s, &r);
    ok("60 초: 다시 변환 시작", o.converts == 2);

    printf("DS18B20 을 빼면 마지막 값에서 150 초 뒤 가스 온도를 BMP280 으로 되돌림\n");
    o.present = false;
    for (g_wall = 61000; g_wall <= 151000; g_wall += 1000) {
        sensors_read(&s, &r);
    }
    ok("마지막 정상값(1 초)에서 150 초까지는 그 값", r.sample.t_probe_ok && r.sample.t_gas_c == 21.4375);
    g_wall = 152000;
    sensors_read(&s, &r);
    ok("넘으면 프로브 없음 · 가스 온도 BMP280", !r.sample.t_probe_ok && fabs(r.sample.t_gas_c - BMP_T_C) < 1e-5);

    printf("SDP810 읽기만 한 번 실패해도 잠 명령은 보냄 (점7)\n");
    {
        x.sdp_read_fail = true;
        const int sl0 = x.sdp_sleeps;
        x.max_consec = 0;
        sensors_read(&s, &r);
        ok("오류 · 값 없음 · 잠 명령 보냄 · 센서 잠듦", r.e_sdp == SENS_E_BUS && !r.sample.dp2_ok && x.sdp_sleeps == sl0 + 1 &&
                                                        x.sdp_asleep);
        ok("실패 이어짐 2번 넘지 않음 (버스 다시 열기 문턱 3 안)", x.max_consec <= 2);
        x.sdp_read_fail = false;
        for (unsigned k = 0; k <= SDP810_RETRY_READS && !r.sample.dp2_ok; k++) {
            sensors_read(&s, &r);
        }
        ok("다음 찾기에서 다시 값이 나옴", r.sample.dp2_ok && r.e_sdp == SENS_OK);
    }

    printf("SDP810 CRC 가 틀리면 값 버리고 그래도 재움\n");
    x.sdp_corrupt = true;
    const int sl = x.sdp_sleeps;
    sensors_read(&s, &r);
    ok("오류 · 값 없음 · 잠 명령 보냄", r.e_sdp == SENS_E_STATUS && !r.sample.dp2_ok && x.sdp_sleeps == sl + 1);
    x.sdp_corrupt = false;

    printf("SDP810 이 없는 판: 60 번 읽을 때마다 한 번만 찾고, 같은 버스의 ABP2 · BMP280 은 그대로\n");
    x.sdp_present = false;
    x.max_consec = 0;
    sensors_read(&s, &r);
    ok("처음 한 번 찾아봄(쓰기 2번 답 없음) → 없음", r.e_sdp == SENS_E_BUS && !r.sample.dp2_ok && x.max_consec == 2);
    const int tr = x.sdp_traffic;
    bool pg_ok = true;
    for (unsigned k = 0; k < SDP810_RETRY_READS; k++) {
        sensors_read(&s, &r);
        pg_ok = pg_ok && r.e_pg == SENS_OK && r.e_baro == SENS_OK;
    }
    ok("다음 60 번 동안 0x25 를 안 건드림", x.sdp_traffic == tr);
    ok("그동안 ABP2 · BMP280 은 늘 OK · 실패 이어짐 2번 넘지 않음", pg_ok && x.max_consec <= 2);
    sensors_read(&s, &r);
    ok("61 번째에 다시 찾아봄", x.sdp_traffic == tr + 2);
    x.sdp_present = true;
    x.sdp_asleep = false;
    for (unsigned k = 0; k <= SDP810_RETRY_READS && !r.sample.dp2_ok; k++) {
        sensors_read(&s, &r);
    }
    ok("다시 꽂으면 다음 찾기에서 값이 나옴", r.sample.dp2_ok && r.e_sdp == SENS_OK);

    printf("ms 시계가 없으면 SDP810 은 50 ms 를 다 기다림\n");
    sensors_set_extra(&s, NULL, NULL, NULL);
    d0 = f->delay_total;
    sensors_read(&s, &r);
    ok("값 OK · 이른 읽기 0 · DS18B20 안 씀", r.e_sdp == SENS_OK && x.sdp_early_reads == 0 && r.e_probe == SENS_E_BUS);
    ok("기다림 ≥ 50 ms", f->delay_total - d0 >= 50u);

    printf("SDP810 헤더 없는 판(PCB3_2 빌드, sdp_off)\n");
    s.sdp_off = true;
    const int tr2 = x.sdp_traffic;
    pg_ok = true;
    for (unsigned k = 0; k < 2u * SDP810_RETRY_READS + 2u; k++) {
        sensors_read(&s, &r);
        pg_ok = pg_ok && r.e_pg == SENS_OK && r.e_baro == SENS_OK && r.e_sdp == SENS_E_BUS && !r.sample.dp2_ok;
    }
    ok("0x25 를 한 번도 안 건드림 · 차압 2 없음 · ABP2 · BMP280 그대로", x.sdp_traffic == tr2 && pg_ok);
    {
        /* 켠 뒤 기압을 아직 한 번도 못 잼 → 대기압 보정을 못 해서 SDP810 값은 안 씀 (해수면 값으로 보정하면 틀림) */
        sensors_t s3;
        sensors_init(&s3, &dp, &pg, fake_power);
        sensors_power_on(&s3);
        s3.bmp_ready = false;
        sensors_result_t r3;
        sensors_read(&s3, &r3);
        ok("기압 못 잰 첫 읽기: 기압은 해수면 대신 값 · SDP810 값은 안 씀", r3.sample.patm_held && !r3.sample.dp2_ok);
        s3.bmp_ready = true;
        sensors_read(&s3, &r3);
        ok("기압을 한 번 재면 SDP810 값을 씀", !r3.sample.patm_held && r3.sample.dp2_ok);
    }
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
    test_bmp280();
    test_sensors_read();
    test_ds18b20();
    test_next_board_sensors();
    test_i2c_timing();
    printf("\n%d / %d 통과\n", checks - fails, checks);
    return fails ? 1 : 0;
}
