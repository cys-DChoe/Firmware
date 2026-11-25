/* 계량기 전체 흐름 — 센서 → 계산 → 1분 기록 저장 → LoRa 보고 → 잠 (설계서 "동작 흐름" 그림)
 *
 * 하드웨어는 app_hw_t 콜백과 이미 열어 둔 링 · 저장 · LoRa 모듈로만 만짐 → 맥에서 가짜 부품으로 통째로 시험함.
 *
 * 한 번 깰 때마다 app_step():
 *   1) 센서 읽기 → 계산(meter_step)
 *   2) LoRa 모듈 소식 처리 (가입됨 · 송신 끝 → 링에 "보냄" 표시)
 *   3) 1분이 넘었으면: 1분 요약 → 24 B 보고 → 링에 저장 → 누적량 저장 → 바로 보냄
 *   4) 보낼 게 없으면 최근 60분 안의 못 보낸 기록을 하나 다시 보냄 (20 s 에 한 번)
 *   5) 가입 안 돼 있으면 10분마다 다시 가입
 *      모듈 소식(송신 끝 60 s · 가입 결과 300 s)이 안 오면 놓친 것으로 보고 풂. 무응답 · 놓침이 3번 이어지면
 *      모듈 리셋 → 속도 · 설정 · 가입을 처음부터. 켤 때 모듈이 답 없었어도 10분마다 다시 해 봄
 *   6) 시계: 가입하면 망에 시각을 물어(AT+TIMEREQ=1) 송신이 끝날 때마다 AT+LTIME 을 읽고,
 *      맞는 시각이 오면 RTC 를 고친 뒤 하루 동안 쉼. 시계가 뛰면 적산 · 분 경계도 같이 옮김
 *   → 다음에 깰 때까지 ms (흐름 있으면 1 s, 없으면 10 s)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "meter.h"
#include "nvlog.h"
#include "persist.h"
#include "rak3172.h"
#include "report.h"
#include "ring.h"
#include "lte.h"

#define APP_LORA_PORT        1u      /* 1분 보고 형식 v1 */
/* 설정 바꾸기 (서버 → 계량기, 내려받기 포트 10) — 형식은 서버 담당과 맞출 때 같이 확정
 *   01 hh ll : 정압 센서 만점 Pa (big-endian, 1000 – 40000). 센서를 다른 범위 품번으로 갈았을 때.
 *   0 이면 기본값(6000)으로. 받으면 바로 적용하고 설정에 저장. */
#define APP_CFG_PORT         10u
#define APP_CFG_PG_FS        0x01u
#define APP_CFG_LTE_TEST     0x02u   /* 02 : LTE 켜기 · 망 확인 · 끄기 한 번 (1차 시험) */
/*   03 hh ll : 방출 기준(물트랩 압력) Pa (big-endian, 100 – 지금 정압 센서 만점). 소화조마다 달라 설치 때 넣음
 *   (물 높이 cm × 98.1 — 10 cm = 981, 20 cm = 1962). 0 이면 기본값(물 20 cm)으로. 받으면 바로 적용 · 저장.
 *   이 값 근처에서 정압이 10분 평평하고 유량이 0 인 분을 '방출'로 셈 (11/18) */
#define APP_CFG_VENT_P       0x03u
#define APP_VENT_P_MIN_PA    100u
/*   04 vv : SDP810 끄기 · 켜기 (vv 00 = 끔, 01 = 켬). PCB3_3 비교 시험이 끝나면 꺼서 전류를 아낌(설계서 판 구분 3번, 11/24).
 *   받으면 바로 적용 · 저장, 같은 값이면 다시 저장 안 함. 판에 SDP810 자리가 없으면(PCB3_2) 저장만 하고 늘 꺼진 그대로 */
#define APP_CFG_SDP          0x04u
/*   05 kk vvvvvvvv [kk vvvvvvvv …] : 교정값 칸 kk 에 float32(큰 쪽 먼저), 한 명령에 1–8칸 — 계량기 한 대마다
 *     (11/24 검수 2번, persist.h cal_t). 같이 바뀌어야 하는 a · b · Re 최소는 한 명령에 담아 보낼 것(검수 2차 중간)
 *     01 a (C = a + b ÷ Re, 0 = 고정 C 0.97) · 02 b · 03 Re 최소(아래는 끝 값) · 04 목 지름 mm · 05 입구 지름 mm
 *     06 가스 점성 20 °C µPa·s · 07 Sutherland S K · 08 가스 분자량 g/mol
 *     kk 에 80 을 더하면(81–88) 그 칸만 펌웨어 기본값으로 (값 4 B 는 아무거나)
 *   05 00 : 교정값을 모두 펌웨어 기본값으로 · 05 FF : 지금 값만 보고
 *   다 넣은 뒤 한 번만 검사: 범위 밖 · C 가 0.5–1.05 를 벗어나면 명령 전체를 무시. 받으면 바로 적용 · 저장(외부 플래시),
 *   결과는 포트 4 로 올림. LoRa 한 번에 41 B — KR920 · EU868 가장 느린 속도(51 B)에도 들어감 */
#define APP_CFG_CAL          0x05u
/* 교정값 보고 (포트 4, 34 B): 형식 1 · 넣은 칸 표시(비트 k = 칸 k+1) · 지금 쓰는 값 8개 float32 큰 쪽 먼저(칸 01–08 순서) */
#define APP_CAL_PORT         4u
#define APP_CAL_LEN          34u
/* LTE 1차 (설계서 5번): 켜고 · 망 등록 확인 · 끄기만. 셀 3.6 V 미만이면 안 함(펌웨어 주의 1절, 0.05 V 여유 → 3.65 V).
 * 결과는 LoRa 포트 2 로 7 바이트 (lte_result_encode) — 1분 보고 링에는 안 넣음 */
#define APP_LTE_PORT         2u
#define APP_LTE_MIN_MV       3650u
/* 횟수 보고 (포트 3, report.h 표): 조리 · 고갈 · 누설 횟수가 바뀌면 보내되 10분에 한 번까지, 안 바뀌어도 하루에 한 번.
 * 방출 분은 방출 중 매분 늘어서 그것만으로는 안 보냄. 켠 뒤 가입하면 한 번 (새 계량기는 첫 1분 보고 뒤).
 * 1분 보고 · LTE 결과보다 뒤, 밀린 기록 다시 보내기보다 앞 */
#define APP_CNT_PORT         3u
#define APP_CNT_GAP_S        600u
#define APP_CNT_DAY_S        86400u
#define APP_LTE_GUARD_S       600u    /* 시키지 않았는데 LTE 가 켜져 있는지 보는 간격 (켤 때 한 번 + 10분마다) */
#define APP_PG_FS_DEFAULT_PA 6000u   /* ABP2DRNT006KG2A3XX (보드 U-S2) */
#define APP_PG_FS_MIN_PA     1000u
#define APP_PG_FS_MAX_PA     40000u
#define APP_RESEND_WINDOW    60u     /* 최근 60분 기록까지 다시 보냄 */
#define APP_RESEND_GAP_S     20u
#define APP_JOIN_RETRY_S     600u
#define APP_ZERO_SAMPLES     16u     /* 영점 = 최근 16번 읽은 차압 평균 */
#define APP_LORA_BAUD        9600u   /* 잠 중에도 받는 속도 (설계서 결정 ⑦) */
#define APP_LORA_BAUD_OLD    115200u /* 모듈 공장값 */
#define APP_TIMESYNC_S       86400u  /* 하루에 한 번 망 시각으로 다시 맞춤 */
#define APP_TIME_STEP_S      2       /* 이만큼 넘게 어긋나면 RTC 를 고침 */
#define APP_PROBE_GRACE_S    120u    /* 켠 뒤 이만큼 DS18B20 값이 없으면 "없음"으로 봄 (첫 변환 + 60 s 간격 한 번 더) */
#define APP_MAX_DT_S         60u     /* 한 번에 적산하는 최대 간격 (시계가 뛰었을 때 막음) */
#define APP_TX_TIMEOUT_S     60u     /* 송신 시작 뒤 '끝' 소식을 기다리는 한도 (SF12 송신 + 수신 창 2개보다 넉넉히) */
#define APP_JOIN_TIMEOUT_S   300u    /* AT+JOIN=1:0:10:8 = 8번 × 10 s 간격 + 수신 창, 넉넉히 */
#define APP_LORA_MISS_MAX    3u      /* 무응답 · 소식 놓침이 이만큼 이어지면 모듈 리셋 후 처음부터 */
/* 셀 3.3 V 종료 (설계서 7번 · 펌웨어 주의 "3.3 셧다운"): MCU 가 끊을 전원 스위치는 없으므로 펌웨어가 쉼.
 *   1분 보고 때 잰 셀이 OFF 아래로 2번 이어지면 그 기록에 BATT_OFF 비트를 붙이고(마지막 보고) →
 *   센서 끄고 측정 · 보고 · 가입을 멈춤. 20 s 마다 깨어 워치독만 밀고, 10분마다 셀을 재서 ON 이상이면 다시 시작.
 *   문턱은 펌웨어 주의의 0.1 V 여유 규칙(3.3 → 3.25 판정)과, 짐을 내린 뒤 셀이 되튀는 폭을 생각한 다시 켬 값.
 *   [가정 — 보드 · 셀에서 다시 정함] */
#define APP_BATT_OFF_MV      3250u
#define APP_BATT_ON_MV       3450u
#define APP_BATT_OFF_COUNT   2u
#define APP_OFF_TICK_MS      20000u  /* 쉬는 동안 깨는 간격 — 워치독(최소 28.6 s)보다 짧게 */
#define APP_OFF_CHECK_S      600u
#define APP_OFF_TX_GRACE_S   120u    /* 쉬기 직전 마지막 보고를 보낼 시간 */
/* LED 상태 표시 (설계서 8번, 11/17): 10 s 마다 짧게 한 번 깜빡 — 늘 켜 두지 않음
 *   평균: LED 2.8 mA × 30 ms / 10 s ≈ 8 µA + 그동안 MCU 가 깨어 있는 몫 → 십수 µA [계산, 보드에서 잼]
 *   초록 1번 = 정상 (센서 읽힘 · LoRa 가입됨 · 마지막 송신 됨)
 *   빨강 1번 = 통신 문제 (키 없음 · 모듈 안 됨 · 가입 안 됨 · 마지막 송신 실패)
 *   빨강 2번 = 센서 문제 (차압 · 정압을 못 읽음 · 기압을 못 재 마지막 값으로 계산 중) — 통신 문제보다 먼저 보임
 *   빨강 3번 = 저장 문제 (1분 기록 · 누적을 저장 못 함 — 그 기록은 못 보냄) — 가장 먼저 보임 (11/18)
 *   셀이 낮아 쉴 때는 안 깜빡임 */
/* 셀 전압 1점 보정 (보드 오면 4단계): 멀티미터로 잰 실제 전압을 주면 지금 잰 값과의 차이를 보정값으로 저장.
 * 나눔 저항 · VREFINT 오차라 크지 않음 → 이보다 크면 잘못 넣은 것으로 보고 안 받음 */
#define APP_BATT_CAL_MAX_MV  300
#define APP_LED_PERIOD_S     10u
#define APP_LED_OK           0u
#define APP_LED_LORA         1u
#define APP_LED_SENSOR       2u
#define APP_LED_STORE        3u

typedef struct {
    bool (*read)(void *ctx, meter_sample_t *s);   /* 센서 읽기, 믿을 만하면 true */
    uint32_t (*now_s)(void *ctx);                  /* RTC Unix 초 */
    bool (*time_valid)(void *ctx);                 /* 시계를 맞췄나 */
    uint16_t (*batt_mv)(void *ctx);                /* 셀 전압, 0 = 아직 못 잼 */
    void (*flash_power)(void *ctx, bool on);       /* 외부 플래시 깨우기 · 재우기 (없으면 NULL) */
    void (*log)(void *ctx, const char *msg);       /* 없으면 NULL */
    void *ctx;
    bool (*set_time)(void *ctx, uint32_t unix_s);  /* RTC 맞추기 (없으면 NULL — 시계 맞추기 안 함) */
    void (*set_pg_fs)(void *ctx, double pa);        /* 정압 센서 만점 바꾸기 (없으면 NULL) */
    void (*power_save)(void *ctx, bool off);        /* 셀 낮아 쉼: 센서 · LED 끄기 / 켜기 (없으면 NULL) */
    void (*led)(void *ctx, bool red, uint8_t blinks); /* 짧게 깜빡 (없으면 NULL) */
    void (*set_sdp)(void *ctx, bool on);            /* SDP810 켜기 · 끄기 (없으면 NULL) */
} app_hw_t;

typedef struct {
    app_hw_t hw;
    meter_t meter;
    ring_t *ring;
    nvlog_t *nv_set, *nv_ctr;
    nvlog_t *nv_cal;            /* 교정값 (외부 플래시, 없으면 NULL — 명령 05 거절) */
    cal_t cal;
    rak_t *rak;
    settings_t settings;
    counter_t counter;
    bool lora_ready;            /* 속도 · 설정까지 끝남 */
    bool started;
    uint32_t last_s;            /* 지난 app_step 시각 */
    uint32_t up_s;              /* 켜진 뒤 흐른 초 — RTC 를 맞춰도 안 뜀 (LTE 단계가 이걸로 잼) */
    meter_sample_t prev;        /* 지난 측정값 — 간격이 길 때 반씩 나눠 적산 */
    bool prev_ok;
    uint32_t last_minute;       /* 마지막으로 보고 만든 분 번호 (Unix 초 / 60) */
    uint16_t boot_flags;        /* 켠 뒤 첫 기록에 붙일 비트 (REPORT_F_BOOT · FAULT_RESET) */
    uint16_t batt_flags;        /* 배터리 비트 오르내림 폭용 */
    uint8_t batt_low_n;         /* 셀 OFF 문턱 아래 연속 횟수 */
    bool batt_off_pending;      /* 이번 1분 기록이 마지막 — 저장 뒤 쉼 */
    bool batt_off;              /* 셀이 낮아 쉬는 중 */
    uint32_t off_since, next_batt_check_s;
    bool read_ok;               /* 마지막 센서 읽기가 쓸 만했나 */
    bool baro_held;             /* 마지막 읽기에서 기압을 못 재 마지막 값으로 씀 */
    bool baro_held_min;         /* 이번 분에 그런 때가 있었음 → 보고 비트 BARO_HELD */
    double sdp_sum, probe_sum;  /* 이번 분 SDP810 차압 · DS18B20 온도 합 (보고 v2 24 · 26 번 = 평균) */
    uint32_t zero_hits_said;    /* 영점 따라가기 한도 닿음을 로그로 알린 횟수 (meter.zero_limit_hits 와 비교) */
    uint8_t probe_state;        /* DS18B20: 0 아직 모름 · 1 있음 · 2 없음 — 바뀐 순간에 로그 한 줄 (설계서 PCB3_3 5번) */
    uint16_t sdp_n, probe_n;
    bool store_failed;          /* 마지막 1분 기록 · 누적 저장이 실패함 (다음 성공까지) */
    bool tx_failed;             /* 마지막 송신이 실패함 (다음 성공까지) */
    uint32_t next_led_s;
    uint8_t led_state;          /* 마지막으로 보인 APP_LED_* */
    lte_t *lte;                 /* 없으면 NULL */
    bool lte_want;              /* LTE 시험을 받아 둠 — 다음 app_step 에서 시작 (그때의 up_s 로 재려고) */
    bool lte_report_pending;    /* LTE 시험 결과를 LoRa 로 올릴 차례 */
    bool cal_report_pending;    /* 교정값 보고(포트 4)를 올릴 차례 */
    bool tx_is_cal;             /* 지금 보내는 것이 교정값 보고 (링 기록 아님) */
    bool tx_is_lte;             /* 지금 보내는 것이 LTE 결과 (링 기록 아님) */
    bool tx_is_cnt;             /* 지금 보내는 것이 횟수 보고 (링 기록 아님) */
    report_counts_t cnt_sent;   /* 마지막으로 보내진 횟수 */
    report_counts_t cnt_sending;
    bool cnt_sent_valid;        /* 켠 뒤 한 번이라도 보냄 */
    uint32_t cnt_last_s, next_cnt_try_s;
    uint32_t cnt_reports;       /* 보내진 횟수 보고 수 (로그 · 시험용) */
    lte_result_t lte_last;
    uint32_t lte_refused;       /* 셀이 낮아 안 한 횟수 */
    uint32_t next_lte_guard;    /* up_s 기준 */
    uint32_t lte_left_on;       /* 켜진 채 발견해서 끈 횟수 */
    bool fresh_pending;         /* 방금 만든 보고를 아직 안 보냄 */
    uint32_t fresh_seq;
    bool tx_live;               /* 모듈이 보내는 중인 기록 */
    uint32_t tx_seq;
    uint32_t next_join_s, next_resend_s;
    bool lora_started;          /* app_lora_start 를 한 번이라도 부름 (그 뒤로 실패하면 다시 해 봄) */
    bool lora_need_reset;       /* 다음에 다시 해 볼 때 모듈 리셋부터 */
    uint32_t next_lora_try_s;
    uint32_t tx_start_s, join_start_s;
    uint8_t lora_misses;        /* 무응답 · 소식 놓침 연속 횟수 */
    bool joined_seen;           /* 가입을 본 뒤 (가입되면 놓침 횟수를 지움) */
    bool timereq_on;            /* 모듈에 시각 요청을 켜 둠 */
    bool time_synced;           /* 이번 켜짐 뒤 망 시각으로 맞춘 적 있음 */
    uint32_t next_timesync_s;
    uint32_t time_syncs;
    double dp_hist[APP_ZERO_SAMPLES];
    uint16_t dp_n, dp_head;
    /* 셈 (로그 · 시험용) */
    uint32_t reports, sent, send_fail, resends, joins, lora_restarts, tx_timeouts, batt_offs;
} app_t;

/* 저장된 설정 · 누적량을 읽어 계산 모듈에 넣음. 누적 기록이 없으면 링 마지막 보고에서 되살림. */
void app_init(app_t *a, const app_hw_t *hw, ring_t *ring, nvlog_t *nv_set, nvlog_t *nv_ctr, rak_t *rak);
/* LoRa: 속도 9600 맞추기 → 키가 있으면 설정 · 가입 시작. 키가 없으면 기록만 쌓음. */
rak_err_t app_lora_start(app_t *a);
/* 한 번 깸. 다음에 깰 때까지 ms 를 돌려줌. */
uint32_t app_step(app_t *a);
/* 버튼 길게: 지금 차압 평균을 영점으로 · 저장 */
bool app_zero(app_t *a);
/* 키 넣기 (처음 설치 때 · secrets.h 에서) — 저장 뒤 다시 설정 · 가입 */
/* 켠 까닭: abnormal = 워치독 · 오류로 재시작함 → 첫 기록에 REPORT_F_FAULT_RESET */
void app_set_boot(app_t *a, bool abnormal);
/* 내려받은 설정 명령 처리 (포트 · 내용). 알아들었으면 true */
bool app_downlink(app_t *a, uint8_t port, const uint8_t *data, uint8_t len);
/* 지금 쓰는 방출 기준 Pa (설정 0 이면 기본값 = 물 20 cm) */
double app_vent_p_pa(const app_t *a);
/* 지금 쓰는 정압 센서 만점 Pa (설정 0 이면 기본값) */
uint16_t app_pg_fs_pa(const app_t *a);
/* LTE 모듈 연결 · 시험 받기 (셀 3.65 V 이상일 때만, 하는 중이면 false). 실제 시작은 다음 app_step 에서 */
void app_set_lte(app_t *a, lte_t *lte);
/* 교정값 자리 연결 · 읽어서 계산에 적용 (app_init 뒤에) */
void app_set_cal(app_t *a, nvlog_t *nv_cal);
double app_cal_value(const app_t *a, unsigned k);          /* 지금 쓰는 값 (넣은 값 또는 기본값, cal_t 단위) */
size_t app_cal_encode(const app_t *a, uint8_t out[APP_CAL_LEN]);
bool app_lte_test(app_t *a);
bool app_set_keys(app_t *a, const uint8_t dev_eui[8], const uint8_t app_eui[8], const uint8_t app_key[16]);
/* 셀 전압 보정: actual_mv = 멀티미터로 잰 셀 전압. 지금 잰 값과 맞도록 보정값을 고쳐 저장 (다시 켜도 유지).
 * 셀을 못 재거나 범위 밖(2.0–4.5 V, 보정 ±300 mV 넘음)이면 false */
bool app_batt_cal(app_t *a, uint16_t actual_mv);
/* 빌드 옵션 BATT_CAL_ACTUAL_MV 용: 보정을 아직 안 넣었을 때만 한 번 맞춤.
 * 그 판을 0 으로 안 되돌려도 켤 때마다 다시 맞추지 않음 — 다시 맞추면 셀이 낮아진 걸 보정값이 덮어 저전압 보호가 안 걸림 (11/18 검수) */
bool app_batt_cal_once(app_t *a, uint16_t actual_mv);
