/* SDP810 대기압 보정 — 보드(Core/Src/sensors.c)와 Nucleo 벤치(bench/nucleo_lora/main.c)가 같이 씀.
 * 벤치는 자기 sdp810.h(연속 측정 0x3615)가 따로 있어 이 식만 이 파일로 뺌 (11/25). */
#pragma once

/* 대기압 보정 (11/24 검수 3번): SDP8xx 는 절대압 966 hPa 에서 교정됨 — 다른 절대압에선 Δp = 읽은 값 × 96,600 ÷ p_abs.
 * Kisii(82.5 kPa)에선 안 하면 14.6 % 작게 읽힘. 벤치(102 kPa)에선 5.6 % 크게(유량 2.8 %).
 * 원문 확인 11/25: SDP8xx 데이터시트 v1.1(2019) 각주 1 "absolute pressure = 966 mbar" · 5.3 절(차압 보상 모드는 절대압 보정 필요,
 * 질량유량 모드 0x3603 · 0x3624 는 불필요) · Sensirion "SDP Signal Compensation" v1.0(2024-06) 3.1 절 dp_cor = dp_sensor × 966 ÷ p_amb.
 * 그 문서는 p_amb(대기압)이라 쓰지만 센서 관 속 기체 밀도는 관 압력을 따르므로 대기압 + 정압을 넣음(정압 만점 6 kPa 면 Kisii 에서 둘 차이 약 7 %).
 * 같은 문서 6 절: 공기 · 질소로 교정됨 — 다른 기체(바이오가스)는 읽은 값이 다를 수 있고 기체별 변환을 따로 만들어야 함 → 현장 SDP810 은 비교용 */
#define SDP810_P_CAL_PA     96600.0

/* 대기압 보정 Δp × 96,600 ÷ p_abs. p_abs 가 30 kPa 이하(못 잼 · 이상)면 그대로 */
static inline double sdp810_p_comp(double dp_pa, double p_abs_pa)
{
    return p_abs_pa > 30000.0 ? dp_pa * SDP810_P_CAL_PA / p_abs_pa : dp_pa;
}
