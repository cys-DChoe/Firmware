/* 1분 보고 묶기 — LoRa 로 보내는 24바이트 (설계서 4번: 항목 10개, 한도 32바이트)
 *
 * 형식 v1, 모든 값은 큰 쪽 먼저(big-endian), 범위를 넘으면 끝값으로 자르고 CLIPPED 표시.
 *
 *  자리  크기  항목            단위 · 눈금                 범위
 *   0    1    형식 버전        1                           —
 *   1    2    순번            0–65535 돌아감               —
 *   3    4    시각            Unix 초 (UTC)                —
 *   7    4    누적 사용량      0.1 L = 0.0001 Nm³          0–429,496 Nm³
 *  11    2    1분 평균 유량    1 L/h (표준)                0–65.5 Nm³/h
 *  13    2    정압            1 Pa, 부호 있음             ±32,767 Pa
 *  15    2    평균 차압        0.05 Pa, 부호 있음 (영점 뺀 값)  ±1,638 Pa
 *  17    2    가스 온도        0.1 °C, 부호 있음           ±3,276 °C
 *  19    2    대기압          1 Pa, 50,000 Pa 를 뺀 값     50,000–115,535 Pa
 *  21    1    배터리          10 mV, 2.5 V 를 뺀 값        2.50–5.05 V
 *  22    2    상태 비트        아래 REPORT_F_*              —
 *
 * 자릿수는 서버 담당과 맞출 때 바꿀 수 있음 — 바꾸면 형식 버전을 올림.
 * 서버 쪽 풀기 참고: tools/report_decode.py
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REPORT_VERSION   1u
#define REPORT_LEN       24u

/* 상태 비트 */
#define REPORT_F_FLOWING        (1u << 0)   /* 이 분에 조리(사용 이벤트) 중이었음 */
#define REPORT_F_VENTING        (1u << 1)   /* 방출 중인 분 */
#define REPORT_F_DEPLETED       (1u << 2)   /* 고갈 상태였음 */
#define REPORT_F_LEAK_B         (1u << 3)   /* 계량기 뒤 누설 판정 중 */
#define REPORT_F_OUT_OF_RANGE   (1u << 4)   /* 차압 · 정압이 센서 만점 95 % 넘음 */
#define REPORT_F_BATT_WARN      (1u << 5)   /* 셀 3.5 V 아래 (경고) */
#define REPORT_F_BATT_QUALITY   (1u << 6)   /* 셀 3.35 V 아래 (품질 B) */
#define REPORT_F_ZERO_UNSET     (1u << 7)   /* 영점을 아직 안 잡음 */
#define REPORT_F_TIME_UNSYNCED  (1u << 8)   /* 시계가 맞춰지지 않음 — 시각 믿지 말 것 */
#define REPORT_F_RESENT         (1u << 9)   /* 못 보냈다가 나중에 다시 보낸 기록 */
#define REPORT_F_CLIPPED        (1u << 10)  /* 어떤 값이 범위를 넘어 잘림 */
#define REPORT_F_INCOMPLETE     (1u << 11)  /* 이 분의 측정 시간이 54 s 미만 */
#define REPORT_F_BOOT           (1u << 12)  /* 켠 뒤 첫 기록 (11/17 추가, 형식 v1 그대로 — 전엔 늘 0) */
#define REPORT_F_FAULT_RESET    (1u << 13)  /* 워치독 · 오류로 재시작한 뒤 첫 기록 */
#define REPORT_F_BATT_OFF       (1u << 14)  /* 셀이 낮아 이 기록 뒤 측정 · 보고를 멈춤 (셀이 오르면 다시 시작) */
#define REPORT_F_BARO_HELD      (1u << 15)  /* 이 분에 기압을 못 잰 때가 있음 — 마지막 값(처음엔 해수면 값)으로 계산 (11/18) */

typedef struct {
    uint16_t seq;
    uint32_t unix_s;
    double total_nm3;       /* 누적 사용량 */
    double flow_nm3h;       /* 1분 평균 표준 유량 */
    double pg_pa;           /* 정압 */
    double dp_pa;           /* 평균 차압 (영점 뺀 값) */
    double t_c;             /* 가스 온도 */
    double patm_pa;         /* 대기압 */
    uint16_t batt_mv;       /* 셀 전압 */
    uint16_t flags;         /* REPORT_F_* */
} report_t;

/* 묶기: out 에 REPORT_LEN 바이트를 씀. 잘린 값이 있으면 CLIPPED 를 켜서 씀. 쓴 길이를 돌려줌. */
size_t report_encode(const report_t *r, uint8_t out[REPORT_LEN]);

/* 풀기: 길이 · 버전이 맞으면 true */
bool report_decode(const uint8_t *in, size_t len, report_t *r);

/* 횟수 보고 (LoRa 포트 3, 11/18) — 1분 보고(24 B · 링 칸 32 B 고정)와 따로 보냄
 *   바이트  크기  내용 (큰 쪽 먼저)
 *    0     1    형식 버전 1
 *    1     2    보낼 때 마지막 1분 보고 순번 (어느 보고 뒤의 횟수인지)
 *    3     4    조리 이벤트 수 (끝난 것)
 *    7     4    가스 고갈 수
 *   11     4    방출 분 수
 *   15     4    누설(계량기 뒤) 판정이 새로 켜진 수
 * 모두 켠 뒤부터가 아니라 처음부터 쌓인 값(저장됨) → 하나를 놓쳐도 다음 것으로 따라잡음. 서버는 앞 값과의 차이로 셈. */
#define REPORT_COUNTS_LEN      19u
#define REPORT_COUNTS_VERSION  1u

typedef struct {
    uint16_t seq;
    uint32_t cooking, depletions, vent_minutes, leaks;
} report_counts_t;

size_t report_counts_encode(const report_counts_t *c, uint8_t out[REPORT_COUNTS_LEN]);
bool report_counts_decode(const uint8_t *in, size_t len, report_counts_t *c);

/* 셀 전압 → 배터리 비트 (오르내림 폭 50 mV: 내려갈 땐 문턱 − 50, 다시 올라갈 땐 문턱 + 50) */
uint16_t report_battery_flags(uint16_t batt_mv, uint16_t prev_flags);
