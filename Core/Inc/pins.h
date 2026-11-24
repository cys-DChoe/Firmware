/* GasMeter_19 PCB3_2 — STM32H563ZGT6 신호 핀 (tools/bom/pin_table.py 가 보드에서 뽑음, 2026-11-16) */
/* LTE UART · DTR · RI · STATUS 핀은 VDDIO2 = 1.8 V (모듈 VDD_EXT). PWRKEY · RST 는 3.3 V → NPN. */
/* 켜고 끄는 순서 · 전압 조건(셀 3.6 V · DTR/RI 3.2 s · VDDIO2 순서 · LoRa RUI3 AT 등)은 Docs/GasMeter_19_펌웨어_주의.html */
#pragma once

#define MODEM_RST_PORT   GPIOE
#define MODEM_RST_PIN    GPIO_PIN_2   /* 1번 · GPIO 출력 — LoRa 모듈(RAK3172) 22번 RST, 낮음 = 리셋 · 3.3 V */
#define SENS_EN_PORT     GPIOE
#define SENS_EN_PIN      GPIO_PIN_3   /* 2번 · GPIO 출력 — 센서 전원(U9 EN) · 3.3 V */
#define BTN_PORT         GPIOC
#define BTN_PIN          GPIO_PIN_13   /* 7번 · GPIO 입력 · EXTI13 — 버튼 · 3.3 V */
#define LSE_IN_PORT      GPIOC
#define LSE_IN_PIN       GPIO_PIN_14   /* 8번 · LSE 32.768 kHz 크리스털 · 3.3 V */
#define LSE_OUT_PORT     GPIOC
#define LSE_OUT_PIN      GPIO_PIN_15   /* 9번 · LSE 32.768 kHz 크리스털 · 3.3 V */
#define LED1_PORT        GPIOF
#define LED1_PIN         GPIO_PIN_3   /* 13번 · GPIO 출력 — LED1 · 3.3 V */
#define LED2_PORT        GPIOF
#define LED2_PIN         GPIO_PIN_4   /* 14번 · GPIO 출력 — LED2 · 3.3 V */
/* 색 (JLC 부품 위치 파일 PCB3_2): D_LED1 = 빨강 LTST-C190KRKT · 1 kΩ(≈1.4 mA), D_LED2 = 초록 LTST-C190KGKT · 470 Ω(≈2.8 mA) */
#define LED_RED_PORT     LED1_PORT
#define LED_RED_PIN      LED1_PIN
#define LED_GREEN_PORT   LED2_PORT
#define LED_GREEN_PIN    LED2_PIN
#define VBAT_ADC_PORT    GPIOA
#define VBAT_ADC_PIN     GPIO_PIN_0   /* 34번 · ADC12_INP0 — 배터리 전압 ÷2 · 3.3 V */
#define SPI1_SCK_PORT    GPIOA
#define SPI1_SCK_PIN     GPIO_PIN_5   /* 41번 · SPI1_SCK — NOR 플래시 · 3.3 V */  /* AF5 */
#define SPI1_MISO_PORT   GPIOA
#define SPI1_MISO_PIN    GPIO_PIN_6   /* 42번 · SPI1_MISO — NOR 플래시 · 3.3 V */  /* AF5 */
#define SPI1_MOSI_PORT   GPIOA
#define SPI1_MOSI_PIN    GPIO_PIN_7   /* 43번 · SPI1_MOSI — NOR 플래시 · 3.3 V */  /* AF5 */
#define FLASH_CS_N_PORT  GPIOC
#define FLASH_CS_N_PIN   GPIO_PIN_4   /* 44번 · GPIO 출력 — 플래시 CS(낮음 = 선택) · 3.3 V */
#define I2C2_SCL_PORT    GPIOB
#define I2C2_SCL_PIN     GPIO_PIN_10   /* 69번 · I2C2_SCL — HSC 차압 · 3.3 V */  /* AF4 */
#define I2C2_SDA_PORT    GPIOB
#define I2C2_SDA_PIN     GPIO_PIN_12   /* 73번 · I2C2_SDA — HSC 차압 · 3.3 V */  /* AF4 */
#define TEMP_DQ_PORT     GPIOB
#define TEMP_DQ_PIN      GPIO_PIN_13   /* 74번 · GPIO 오픈 드레인 — DS18B20 1-Wire(TEMP_DQ → R_TMP 100 Ω → J_TMP 3번) · 3.3 V · 다음 판(PCB3_3)부터, PCB3_2 는 빈 핀 */
#define ABP2_EOC_PORT    GPIOB
#define ABP2_EOC_PIN     GPIO_PIN_14   /* 75번 · GPIO 입력 · EXTI14 — ABP2 변환 끝 · 3.3 V */
#define DBG_TX_PORT      GPIOD
#define DBG_TX_PIN       GPIO_PIN_8   /* 77번 · USART3_TX — 디버그 헤더 · 3.3 V */  /* AF7 */
#define DBG_RX_PORT      GPIOD
#define DBG_RX_PIN       GPIO_PIN_9   /* 78번 · USART3_RX — 디버그 헤더 · 3.3 V */  /* AF7 */
#define BMP_INT_PORT     GPIOG
#define BMP_INT_PIN      GPIO_PIN_7   /* 92번 · GPIO 입력 · EXTI7 — BMP390 인터럽트 · 3.3 V */
#define I2C3_SDA_PORT    GPIOC
#define I2C3_SDA_PIN     GPIO_PIN_9   /* 99번 · I2C3_SDA — ABP2 · BMP390 · 3.3 V */  /* AF4 */
#define I2C3_SCL_PORT    GPIOA
#define I2C3_SCL_PIN     GPIO_PIN_8   /* 100번 · I2C3_SCL — ABP2 · BMP390 · 3.3 V */  /* AF4 */
#define SWDIO_PORT       GPIOA
#define SWDIO_PIN        GPIO_PIN_13   /* 105번 · SWDIO · 3.3 V */  /* AF0 */
#define SWCLK_PORT       GPIOA
#define SWCLK_PIN        GPIO_PIN_14   /* 109번 · SWCLK · 3.3 V */  /* AF0 */
#define LTE_RXD_PORT     GPIOG
#define LTE_RXD_PIN      GPIO_PIN_9   /* 124번 · USART6_RX ← 모듈 MAIN_TXD(35번) · VDDIO2 1.8 V */  /* AF7 */
#define LTE_RI_PORT      GPIOG
#define LTE_RI_PIN       GPIO_PIN_11   /* 126번 · GPIO 입력 · EXTI11 ← 모듈 RI · VDDIO2 1.8 V */
#define LTE_RTS_PORT     GPIOG
#define LTE_RTS_PIN      GPIO_PIN_12   /* 127번 · USART6_RTS → 모듈 MAIN_RTS · VDDIO2 1.8 V */  /* AF7 */
#define LTE_CTS_PORT     GPIOG
#define LTE_CTS_PIN      GPIO_PIN_13   /* 128번 · USART6_CTS ← 모듈 MAIN_CTS · VDDIO2 1.8 V */  /* AF7 */
#define LTE_TXD_PORT     GPIOG
#define LTE_TXD_PIN      GPIO_PIN_14   /* 129번 · USART6_TX → 모듈 MAIN_RXD(34번) · VDDIO2 1.8 V */  /* AF7 */
#define LTE_RST_PORT     GPIOG
#define LTE_RST_PIN      GPIO_PIN_15   /* 132번 · GPIO 출력 — NPN 으로 RESET_N 당김(높음 = 리셋) · 3.3 V */
#define LTE_PWRKEY_PORT  GPIOB
#define LTE_PWRKEY_PIN   GPIO_PIN_5   /* 135번 · GPIO 출력 — NPN 으로 PWRKEY 당김(높음 = 누름) · 3.3 V */
#define MODEM_RX_PORT    GPIOB
#define MODEM_RX_PIN     GPIO_PIN_6   /* 136번 · LPUART1_TX(AF8) 또는 USART1_TX(AF7) → RAK3172 1번 UART2_RX(AT 115200) · 3.3 V */  /* AF8 */
#define MODEM_TX_PORT    GPIOB
#define MODEM_TX_PIN     GPIO_PIN_7   /* 137번 · LPUART1_RX(AF8) 또는 USART1_RX(AF7) ← RAK3172 2번 UART2_TX(AT 115200) · 3.3 V */  /* AF8 */
#define LTE_DTR_PORT     GPIOB
#define LTE_DTR_PIN      GPIO_PIN_8   /* 139번 · GPIO 출력 → 모듈 DTR(절전 깨우기) · VDDIO2 1.8 V */
#define LTE_STATUS_PORT  GPIOB
#define LTE_STATUS_PIN   GPIO_PIN_9   /* 140번 · GPIO 입력 · EXTI9 ← 모듈 STATUS · VDDIO2 1.8 V */
