/* nvlog 를 MCU 내장 고내구 데이터 플래시(EDATA)에 올리는 연결 — BOM 메모 "적산 · 설정 · 키는 MCU 안 고내구 데이터 플래시"
 *
 * 근거 (RM0481 7장 · ST 예제 STM32CubeH5 FLASH_EDATA_EraseProgram · ST 커뮤니티 답):
 *   - 1 MB 판(H563ZG)도 EDATA 는 은행마다 섹터 120–127 이고 코드 섹터 0–63 과 겹치지 않음 (RM0481 그림 26)
 *   - 은행 1 EDATA = 0x0900_0000–0x0900_BFFF, 섹터 하나 6 KB(0x1800): 섹터 120 = 0x0900_0000 … 127 = 0x0900_A800
 *   - 옵션 바이트 EDATA1R: 마지막 N 섹터를 EDATA 로 (N = 4 → 섹터 124–127 = 0x0900_6000–0x0900_BFFF)
 *   - 지우기 = 보통 섹터 지우기에 120+k, 쓰기 = 16비트(FLASH_TYPEPROGRAM_HALFWORD_EDATA), 읽기 16 · 32비트만
 *   - 한 번도 안 쓴 16비트를 읽으면 ECC 이중 오류 → NMI, 읽힌 값은 0xFFFF, ECCDR = 0xFFFF (NMI 에서 지우고 돌아감)
 *   - 16비트마다 6비트 ECC → 지운 뒤 한 번만 씀
 * 은행 2 는 설계서 9번(원격 갱신 자리)으로 비워 둠 → 은행 1 만 씀.
 */
#pragma once

#include <stdbool.h>
#include "nvlog.h"

#define NV_EDATA_SECTORS       4u          /* 은행 1 마지막 4섹터 = 124–127 */
#define NV_EDATA_SECTOR_SIZE   0x1800u     /* 6 KB */
#define NV_EDATA_BASE_ADDR     (0x09000000u + (8u - NV_EDATA_SECTORS) * NV_EDATA_SECTOR_SIZE)   /* 0x0900_6000 */
#define NV_EDATA_FIRST_SECTOR  (128u - NV_EDATA_SECTORS)                                          /* 124 */

/* nvlog 안의 자리 (위 4섹터 안에서의 오프셋) */
#define NV_EDATA_SETTINGS_BASE 0u
#define NV_EDATA_COUNTER_BASE  (2u * NV_EDATA_SECTOR_SIZE)

typedef enum {
    NV_EDATA_READY = 0,
    NV_EDATA_FAILED,
} nv_edata_state_t;

/* EDATA 가 켜져 있는지 보고 아니면 옵션 바이트로 켬(처음 한 번). 캐시 안 하게 MPU 설정. */
nv_edata_state_t nv_edata_init(void);
void nv_edata_ops(flash_ops_t *ops);
/* NMI 에서 부름: 안 쓴 칸 · 깨진 칸 읽기로 난 ECC 이중 오류면 지우고 true */
bool nv_edata_nmi(void);
