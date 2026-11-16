#include "batt.h"
#include "batt_stm32.h"
#include "main.h"

#define SAMPLES 16u

static ADC_HandleTypeDef s_adc;

static uint32_t read_avg(uint32_t channel)
{
    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel = channel;
    ch.Rank = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;   /* 나눔 저항 50 kΩ 쪽이라 길게 (8 MHz 에서 80 µs) */
    ch.SingleDiff = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    if (HAL_ADC_ConfigChannel(&s_adc, &ch) != HAL_OK) {
        return 0;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < SAMPLES; i++) {
        if (HAL_ADC_Start(&s_adc) != HAL_OK || HAL_ADC_PollForConversion(&s_adc, 10) != HAL_OK) {
            return 0;
        }
        sum += HAL_ADC_GetValue(&s_adc);
    }
    HAL_ADC_Stop(&s_adc);
    return (sum + SAMPLES / 2u) / SAMPLES;
}

uint16_t batt_measure_mv(int16_t cal_offset_mv)
{
    s_adc.Instance = ADC1;
    s_adc.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV4;        /* 커널 클럭 HCLK 32 MHz ÷ 4 = 8 MHz */
    s_adc.Init.Resolution = ADC_RESOLUTION_12B;
    s_adc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    s_adc.Init.ScanConvMode = ADC_SCAN_DISABLE;
    s_adc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    s_adc.Init.LowPowerAutoWait = DISABLE;
    s_adc.Init.ContinuousConvMode = DISABLE;
    s_adc.Init.NbrOfConversion = 1;
    s_adc.Init.DiscontinuousConvMode = DISABLE;
    s_adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    s_adc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    s_adc.Init.DMAContinuousRequests = DISABLE;
    s_adc.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    s_adc.Init.OversamplingMode = DISABLE;
    if (HAL_ADC_Init(&s_adc) != HAL_OK) {
        return 0;
    }
    uint16_t mv = 0;
    if (HAL_ADCEx_Calibration_Start(&s_adc, ADC_SINGLE_ENDED) == HAL_OK) {
        const uint32_t vref = read_avg(ADC_CHANNEL_VREFINT);
        const uint32_t cell = read_avg(ADC_CHANNEL_0);
        if (vref != 0u && cell != 0u) {
            mv = batt_cell_mv((uint16_t)cell, batt_vdda_mv((uint16_t)vref, *VREFINT_CAL_ADDR), cal_offset_mv);
        }
    }
    HAL_ADC_DeInit(&s_adc);                                  /* 깊은 절전으로 */
    return mv;
}
