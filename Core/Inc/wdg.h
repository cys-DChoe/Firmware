/* 워치독 · 오류 재시작 (11/17 결정: 워치독 30초 + 오류면 재시작)
 *
 * - IWDG(LSI 32 kHz ÷ 256, 3750 → 30 s): 주 루프가 깰 때마다(1 · 10 s) 밀어 줌. 30 s 넘게 멈추면 재시작.
 *   Stop 중에도 셈(옵션 바이트 기본값) → 깨는 간격이 30 s 보다 짧아야 함. 디버거로 멈추면 같이 멈춤.
 * - Error_Handler · HardFault · 알 수 없는 NMI: 원인을 RTC 백업 칸(TAMP BKP0–3)에 적고 재시작.
 *   디버거가 붙어 있으면 예전처럼 LED2 켜고 그 자리에 멈춤(보려고).
 * - 이상 재시작이 3번 이어지면(켤 때 고장 등) 바로 다시 켜지 않고 Standby 로 잠 → 워치독이 30 s 뒤 깨움.
 *   배터리를 빨리 닳게 하는 재시작 반복을 막음. 정상으로 10분(잔 시간 합) 돌면 횟수를 지움.
 *   쉬고 난 뒤에도 켠 까닭은 '오류'로 읽힘(백업 칸 표식).
 * [가정 — 보드에서 확인] IWDG 가 Stop · Standby 중에도 도는 것(옵션 바이트 IWDG_STOP · IWDG_STDBY 기본 1). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WDG_CAUSE_POWER = 0,     /* 전원 켬 · 전압 떨어짐(BOR) */
    WDG_CAUSE_PIN,           /* 리셋 핀(버튼 · 디버거) */
    WDG_CAUSE_IWDG,          /* 워치독 — 어딘가 30 s 넘게 멈춤 */
    WDG_CAUSE_FAULT,         /* 오류 처리 · HardFault 에서 재시작 (code · info 참고) */
    WDG_CAUSE_SOFT,          /* 그 밖의 소프트웨어 재시작 */
    WDG_CAUSE_OTHER,
} wdg_cause_t;

/* 오류 코드 (BKP0 아래 8비트) */
#define WDG_FAULT_ERROR   1u    /* Error_Handler — info = 부른 곳 주소 */
#define WDG_FAULT_HARD    2u    /* HardFault 와 그 형제들 — info = 멈춘 PC, cfsr = 원인 레지스터 */
#define WDG_FAULT_NMI     3u    /* EDATA 아닌 NMI */
#define WDG_FAULT_EXIT    4u    /* _exit (abort) */

typedef struct {
    wdg_cause_t cause;
    uint8_t code;            /* FAULT 일 때 WDG_FAULT_* */
    uint32_t info;           /* 주소 */
    uint32_t cfsr;
    uint32_t streak;         /* 이어진 이상 재시작 수 (이번 포함) */
} wdg_boot_t;

wdg_boot_t wdg_boot_cause(void);     /* 켠 직후 한 번: 원인 읽고 표시 지움 */
bool wdg_abnormal(const wdg_boot_t *b);
const char *wdg_cause_name(wdg_cause_t c);
void wdg_start(void);
void wdg_kick(void);
void wdg_healthy(void);              /* 정상으로 오래 돌았음 → 이어진 횟수 지움 */
void wdg_fault(uint32_t code, uint32_t info, uint32_t cfsr) __attribute__((noreturn));
