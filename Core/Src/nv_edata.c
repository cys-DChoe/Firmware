#include "main.h"
#include "nv_edata.h"

static volatile uint32_t s_ecc_reads;     /* 안 쓴 칸 · 깨진 칸을 읽어 난 ECC 오류 수 (로그용) */

static uint32_t abs_addr(uint32_t off) { return NV_EDATA_BASE_ADDR + off; }

/* 16비트씩만 읽음 (8 · 64비트 읽기는 버스 오류) */
static int rd(void *ctx, uint32_t off, uint8_t *b, size_t n)
{
    (void)ctx;
    if ((off & 1u) != 0u || (n & 1u) != 0u || off + n > NV_EDATA_SECTORS * NV_EDATA_SECTOR_SIZE) {
        return -1;
    }
    for (size_t i = 0; i < n; i += 2u) {
        const uint16_t v = *(volatile const uint16_t *)abs_addr(off + (uint32_t)i);
        b[i] = (uint8_t)v;
        b[i + 1u] = (uint8_t)(v >> 8);
    }
    return 0;
}

static int pg(void *ctx, uint32_t off, const uint8_t *b, size_t n)
{
    (void)ctx;
    if ((off & 1u) != 0u || (n & 1u) != 0u || off + n > NV_EDATA_SECTORS * NV_EDATA_SECTOR_SIZE) {
        return -1;
    }
    int e = 0;
    HAL_FLASH_Unlock();
    for (size_t i = 0; i < n && e == 0; i += 2u) {
        const uint16_t hw = (uint16_t)(b[i] | b[i + 1u] << 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD_EDATA, abs_addr(off + (uint32_t)i), (uint32_t)&hw) != HAL_OK) {
            e = -1;
        }
    }
    HAL_FLASH_Lock();
    return e;
}

static int er(void *ctx, uint32_t off)
{
    (void)ctx;
    if (off % NV_EDATA_SECTOR_SIZE != 0u || off >= NV_EDATA_SECTORS * NV_EDATA_SECTOR_SIZE) {
        return -1;
    }
    FLASH_EraseInitTypeDef ei = {0};
    uint32_t bad = 0;
    ei.TypeErase = FLASH_TYPEERASE_SECTORS;
    ei.Banks = FLASH_BANK_1;
    ei.Sector = NV_EDATA_FIRST_SECTOR + off / NV_EDATA_SECTOR_SIZE;   /* 보통 섹터 번호 120–127 로 지움 */
    ei.NbSectors = 1;
    HAL_FLASH_Unlock();
    const HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&ei, &bad);
    HAL_FLASH_Lock();
    return st == HAL_OK ? 0 : -1;
}

void nv_edata_ops(flash_ops_t *ops)
{
    ops->read = rd;
    ops->program = pg;
    ops->erase = er;
    ops->sector_size = NV_EDATA_SECTOR_SIZE;
    ops->ctx = NULL;
}

nv_edata_state_t nv_edata_init(void)
{
    FLASH_OBProgramInitTypeDef ob = {0};
    ob.Banks = FLASH_BANK_1;
    HAL_FLASHEx_OBGetConfig(&ob);
    if ((ob.OptionType & OPTIONBYTE_EDATA) == 0u || ob.EDATASize != NV_EDATA_SECTORS) {
        /* 처음 한 번: 은행 1 마지막 4섹터를 EDATA 로. 옵션 바이트를 적용하면 MCU 가 다시 켜짐 */
        FLASH_OBProgramInitTypeDef w = {0};
        w.OptionType = OPTIONBYTE_EDATA;
        w.Banks = FLASH_BANK_1;
        w.EDATASize = NV_EDATA_SECTORS;
        HAL_FLASH_Unlock();
        HAL_FLASH_OB_Unlock();
        const HAL_StatusTypeDef st = HAL_FLASHEx_OBProgram(&w);
        if (st == HAL_OK) {
            HAL_FLASH_OB_Launch();          /* 옵션 바이트 적용 (MCU 가 다시 켜질 수도 있음) */
        }
        HAL_FLASH_OB_Lock();
        HAL_FLASH_Lock();
        FLASH_OBProgramInitTypeDef chk = {0};
        chk.Banks = FLASH_BANK_1;
        HAL_FLASHEx_OBGetConfig(&chk);
        if (st != HAL_OK || chk.EDATASize != NV_EDATA_SECTORS) {
            return NV_EDATA_FAILED;
        }
    }
    /* 캐시 안 하기(MPU)는 main.c mpu_init() 에서 켜자마자 함 */
    return NV_EDATA_READY;
}

bool nv_edata_nmi(void)
{
    if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_ECCD) == 0u) {
        return false;
    }
    /* RM0481: ECCD 면 ICACHE 를 무효화. 안 쓴 칸이면 ECCDR = 0xFFFF, 아니면 쓰다 끊긴 칸 —
     * 둘 다 읽힌 값으로 CRC 가 틀려 nvlog 가 버리므로 여기선 지우고 돌아감 */
    HAL_ICACHE_Invalidate();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCD);
    s_ecc_reads++;
    return true;
}
