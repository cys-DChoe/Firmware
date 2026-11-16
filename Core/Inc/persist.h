/* 정전 뒤에도 남겨야 하는 값 두 가지 — nvlog 에 따로 저장
 *   설정(settings): 영점 · 방출 문턱(설치 입력) · LoRa 대역 · 가입 키 — 거의 안 바뀜
 *   계수(counter): 누적 사용량 · 순번 · 방출 분 · 고갈 횟수 · 이벤트 수 — 1분마다
 * 바이트로 바꿀 때 순서 · 크기를 고정(작은 쪽 먼저)해서 컴파일러 · 판이 달라도 같은 모양.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nvlog.h"

#define PERSIST_SETTINGS_LEN  50u
#define PERSIST_COUNTER_LEN   38u

typedef struct {
    double zero_pa;          /* 영점 Z */
    double vent_p_pa;        /* 방출 문턱 P_vent (물트랩 높이 × 98.1), 0 = 아직 안 넣음 */
    uint8_t band;            /* LoRa 대역 7 = KR920 · 4 = EU868 */
    bool zero_set;
    bool keys_set;
    uint8_t dev_eui[8], app_eui[8], app_key[16];
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
} counter_t;

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

/* 외부 플래시 자리 (W25Q128 위 4 MB, 링 12 MB 바로 뒤) */
#define PERSIST_SETTINGS_BASE  0xC00000u   /* 섹터 2개 */
#define PERSIST_COUNTER_BASE   0xC02000u   /* 섹터 2개 */
