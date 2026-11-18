cmake_minimum_required(VERSION 3.22)

# 맥: source ~/toolchains/env.sh
#     cmake -S . -B build.nosync -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#     cmake --build build.nosync
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi.cmake)
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

project(gasmeter C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(DEV ${TP}/cmsis-device-h5)
set(HAL ${TP}/stm32h5xx-hal-driver)

# 쓰는 HAL 모듈만 컴파일 (Core/Inc/stm32h5xx_hal_conf.h 에서 켠 것과 맞춤)
set(HAL_SOURCES
  ${HAL}/Src/stm32h5xx_hal.c
  ${HAL}/Src/stm32h5xx_hal_adc.c
  ${HAL}/Src/stm32h5xx_hal_adc_ex.c
  ${HAL}/Src/stm32h5xx_hal_cortex.c
  ${HAL}/Src/stm32h5xx_hal_dma.c
  ${HAL}/Src/stm32h5xx_hal_dma_ex.c
  ${HAL}/Src/stm32h5xx_hal_exti.c
  ${HAL}/Src/stm32h5xx_hal_flash.c
  ${HAL}/Src/stm32h5xx_hal_flash_ex.c
  ${HAL}/Src/stm32h5xx_hal_gpio.c
  ${HAL}/Src/stm32h5xx_hal_i2c.c
  ${HAL}/Src/stm32h5xx_hal_i2c_ex.c
  ${HAL}/Src/stm32h5xx_hal_icache.c
  ${HAL}/Src/stm32h5xx_hal_pwr.c
  ${HAL}/Src/stm32h5xx_hal_pwr_ex.c
  ${HAL}/Src/stm32h5xx_hal_rcc.c
  ${HAL}/Src/stm32h5xx_hal_rcc_ex.c
  ${HAL}/Src/stm32h5xx_hal_rtc.c
  ${HAL}/Src/stm32h5xx_hal_rtc_ex.c
  ${HAL}/Src/stm32h5xx_hal_spi.c
  ${HAL}/Src/stm32h5xx_hal_spi_ex.c
  ${HAL}/Src/stm32h5xx_hal_uart.c
  ${HAL}/Src/stm32h5xx_hal_uart_ex.c
)

add_executable(${PROJECT_NAME}.elf
  Core/Src/main.c
  Core/Src/meter.c
  Core/Src/report.c
  Core/Src/hsc.c
  Core/Src/abp2.c
  Core/Src/bmp390.c
  Core/Src/sensors.c
  Core/Src/i2c_stm32.c
  Core/Src/i2c_timing.c
  Core/Src/rak3172.c
  Core/Src/lora_uart_stm32.c
  Core/Src/w25q.c
  Core/Src/ring.c
  Core/Src/flash_spi_stm32.c
  Core/Src/lowpower.c
  Core/Src/lowpower_stm32.c
  Core/Src/nvlog.c
  Core/Src/persist.c
  Core/Src/nv_w25q.c
  Core/Src/nv_edata.c
  Core/Src/app.c
  Core/Src/batt.c
  Core/Src/wdg_stm32.c
  Core/Src/lte.c
  Core/Src/lte_uart_stm32.c
  Core/Src/batt_stm32.c
  Core/Src/log.c
  Core/Src/syscalls.c
  Core/Src/stm32h5xx_it.c
  Core/Src/stm32h5xx_hal_msp.c
  ${DEV}/Source/Templates/system_stm32h5xx.c
  ${DEV}/Source/Templates/gcc/startup_stm32h563xx.s
  ${HAL_SOURCES}
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE STM32H563xx USE_HAL_DRIVER)
option(LTE_TEST_AT_BOOT "켤 때 LTE 시험을 한 번 함 (벤치용)" OFF)
if(LTE_TEST_AT_BOOT)
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE LTE_TEST_AT_BOOT)
endif()
set(BATT_CAL_ACTUAL_MV 0 CACHE STRING "셀 보정: 멀티미터로 잰 셀 전압 mV (0 = 안 함, 벤치용 — 끝나면 0 으로)")
if(BATT_CAL_ACTUAL_MV GREATER 0)
  target_compile_definitions(${PROJECT_NAME}.elf PRIVATE BATT_CAL_ACTUAL_MV=${BATT_CAL_ACTUAL_MV})
endif()
target_include_directories(${PROJECT_NAME}.elf PRIVATE
  Core/Inc
  ${DEV}/Include
  ${HAL}/Inc
  ${TP}/cmsis_core/Include
)
target_compile_options(${PROJECT_NAME}.elf PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-unused-parameter>
  $<$<CONFIG:Debug>:-Og -g3>
  $<$<CONFIG:Release>:-Os -g>
)
target_link_libraries(${PROJECT_NAME}.elf PRIVATE m)
set(LDSCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/board/STM32H563ZGTX_FLASH.ld)
target_link_options(${PROJECT_NAME}.elf PRIVATE
  -T${LDSCRIPT}
  -Wl,-Map=${PROJECT_NAME}.map,--cref
  -Wl,--print-memory-usage
  -u _printf_float          # 로그에 누적량 · 영점 소수 찍기 (newlib-nano 는 기본으로 빠져 있음)
)
set_target_properties(${PROJECT_NAME}.elf PROPERTIES LINK_DEPENDS ${LDSCRIPT})

add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
)
