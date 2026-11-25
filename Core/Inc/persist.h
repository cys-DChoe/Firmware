/* 정전 뒤에도 남겨야 하는 값 두 가지 — nvlog 에 따로 저장
 *   설정(settings): 영점 · 방출 문턱(설치 입력) · LoRa 대역 · 가입 키 · 셀 전압 보정 — 거의 안 바뀜
 *   계수(counter): 누적 사용량 · 순번 · 방출 분 · 고갈 횟수 · 이벤트 수 — 1분마다
 * 바이트로 바꿀 때 순서 · 크기를 고정(작은 쪽 먼저)해서 컴파일러 · 판이 달라도 같은 모양.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nvlog.h"

#define PERSIST_SETTINGS_LEN  54u   /* 옛 50 · 52 B 판도 읽힘: 빠진 칸은 0 */
#define PERSIST_COUNTER_LEN   43u   /* 옛 38 · 42 B 판도 읽힘: 빠진 칸은 0 (11/18) */
#define PERSIST_CAL_LEN       34u   /* 교정값: 형식 1 · 칸 표시 1 · float 8개 (11/24) */
#define CAL_N                 8u

typedef struct {
    double zero_pa;          /* 영점 Z */
    double vent_p_pa;        /* 방출 문턱 P_vent (물트랩 높이 × 98.1), 0 = 아직 안 넣음 */
    uint8_t band;            /* LoRa 대역 7 = KR920 · 4 = EU868 */
    bool zero_set;
    bool keys_set;
    bool batt_cal_set;       /* 셀 보정을 한 번 넣었음 (빌드 옵션으로 켤 때마다 다시 맞추지 않게, 11/18) */
    uint8_t dev_eui[8], app_eui[8], app_key[16];
    int16_t batt_cal_mv;     /* 셀 전압 1점 보정값 (생산 때 실제 전압 − 잰 값) */
    uint16_t pg_fs_pa;       /* 정압 센서 만점 Pa — 0 = 기본(ABP2 006KG, 6000). 2.5 kPa 품번으로 바꾸면 2500 (11/17) */
    bool sdp_off;            /* SDP810 끔 (설정 04 00, 11/24) — 기본 0 = 켬. 저장 칸이 54 B 꽉 차서 17번 플래그 바이트의 비트 3 */
} settings_t;

typedef struct {
    double total_nm3;
    double total_m3;
    uint32_t ring_next_seq;  /* 링 순번 (확인용) */
    uint16_t report_seq;     /* 다음 1분 보고 순번 */
    uint32_t vent_minutes;
    uint32_t depletions;
    uint32_t events;
    uint32_t unix_s;         /* 저장한 시각 */
    uint32_t leaks;          /* 누설(계량기 뒤) 켜진 횟수 */
    bool leak_active;        /* 지금 누설 판정 중 — 다시 켠 뒤 같은 누설을 또 세지 않게 */
} counter_t;

/* 계량기 한 대의 교정값 (11/24 검수 2번 — 설계서 2번 "설정값" · 대당 교정). 설정 명령 05 로 넣음.
 * 칸 표시(has)의 비트가 꺼진 칸은 펌웨어 기본값(meter_default_cfg)을 씀 — 기본값이 바뀌면 그대로 따라감.
 * 단위는 사람이 읽기 쉬운 것으로: 목 · 입구 mm, 점성 µPa·s, 분자량 g/mol */
enum {
    CAL_C_RE_A = 0,     /* C = a + b ÷ Re 의 a (0 = C(Re) 끔, 고정 C 0.97) */
    CAL_C_RE_B,         /* b */
    CAL_C_RE_MIN,       /* 이 Re 아래는 끝 값 */
    CAL_THROAT_MM,      /* 목 지름 */
    CAL_INLET_MM,       /* 입구 지름 */
    CAL_VISC_UPAS,      /* 가스 점성 20 °C, µPa·s */
    CAL_VISC_S_K,       /* Sutherland S */
    CAL_MOLAR_GMOL,     /* 가스 평균 분자량 */
};
typedef struct {
    uint8_t has;        /* 비트 k = v[k] 를 넣었음 */
    float v[CAL_N];
} cal_t;

void settings_default(settings_t *s);
size_t settings_pack(const settings_t *s, uint8_t *out);   /* PERSIST_SETTINGS_LEN 바이트 */
void settings_unpack(const uint8_t *in, settings_t *s);
size_t counter_pack(const counter_t *c, uint8_t *out);     /* PERSIST_COUNTER_LEN 바이트 */
void counter_unpack(const uint8_t *in, counter_t *c);

/* nvlog 위에서 바로 읽고 쓰기 */
nvlog_err_t settings_load(const nvlog_t *l, settings_t *s);   /* 없으면 기본값 + NVLOG_E_EMPTY */
nvlog_err_t settings_save(nvlog_t *l, const settings_t *s);
nvlog_err_t counter_load(const nvlog_t *l, counter_t *c);
nvlog_err_t counter_save(nvlog_t *l, const counter_t *c);
size_t cal_pack(const cal_t *c, uint8_t *out);              /* PERSIST_CAL_LEN 바이트 */
void cal_unpack(const uint8_t *in, cal_t *c);
nvlog_err_t cal_load(const nvlog_t *l, cal_t *c);           /* 없으면 has 0 + NVLOG_E_EMPTY */
nvlog_err_t cal_save(nvlog_t *l, const cal_t *c);

/* 외부 플래시 자리 (W25Q128 위 4 MB, 링 12 MB 바로 뒤) */
#define PERSIST_SETTINGS_BASE  0xC00000u   /* 섹터 2개 */
#define PERSIST_COUNTER_BASE   0xC02000u   /* 섹터 2개 */
#define PERSIST_CAL_BASE       0xC04000u   /* 섹터 2개 — 교정값은 늘 외부 플래시(EDATA 4섹터는 설정 · 계수로 꽉 참, 거의 안 바뀜) */
