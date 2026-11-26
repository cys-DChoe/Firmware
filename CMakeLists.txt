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

# ---------- 보드 펌웨어: 판마다 따로 (boards/<판>/board.h · pins.h) ----------
# 공통 코드(Core/)는 하나, 판마다 다른 것(핀 · 있는 센서)만 boards/ 폴더에 둠.
#   gasmeter_pcb3_2.elf — 11/18 주문한 판 (기본, 보드 오면 이것부터)
#   gasmeter_pcb3_3.elf — 다음 판 (SDP810 · DS18B20 헤더, PA9 GND)
# 한 판만: cmake --build build.nosync --target gasmeter_pcb3_3.elf
set(GASMETER_SOURCES
  Core/Src/main.c
  Core/Src/meter.c
  Core/Src/report.c
  Core/Src/hsc.c
  Core/Src/abp2.c
  Core/Src/bmp280.c
  Core/Src/sensors.c
  Core/Src/sdp810.c
  Core/Src/ds18b20.c
  Core/Src/onewire_stm32.c
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
option(LTE_TEST_AT_BOOT "켤 때 LTE 시험을 한 번 함 (벤치용)" OFF)
set(BATT_CAL_ACTUAL_MV 0 CACHE STRING "셀 보정: 멀티미터로 잰 셀 전압 mV (0 = 안 함, 벤치용 — 끝나면 0 으로)")
set(LDSCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/boards/STM32H563ZGTX_FLASH.ld)   # 두 판 같은 MCU · 같은 링커 스크립트

function(add_gasmeter BOARD)
  set(T gasmeter_${BOARD}.elf)
  set(B gasmeter_${BOARD})
  add_executable(${T} ${GASMETER_SOURCES})
  target_compile_definitions(${T} PRIVATE STM32H563xx USE_HAL_DRIVER)
  if(LTE_TEST_AT_BOOT)
    target_compile_definitions(${T} PRIVATE LTE_TEST_AT_BOOT)
  endif()
  if(BATT_CAL_ACTUAL_MV GREATER 0)
    target_compile_definitions(${T} PRIVATE BATT_CAL_ACTUAL_MV=${BATT_CAL_ACTUAL_MV})
  endif()
  target_include_directories(${T} PRIVATE
    boards/${BOARD}
    Core/Inc
    ${DEV}/Include
    ${HAL}/Inc
    ${TP}/cmsis_core/Include
  )
  target_compile_options(${T} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-unused-parameter>
    $<$<CONFIG:Debug>:-Og -g3>
    $<$<CONFIG:Release>:-Os -g>
  )
  target_link_libraries(${T} PRIVATE m)
  target_link_options(${T} PRIVATE
    -T${LDSCRIPT}
    -Wl,-Map=${B}.map,--cref
    -Wl,--print-memory-usage
    -u _printf_float          # 로그에 누적량 · 영점 소수 찍기 (newlib-nano 는 기본으로 빠져 있음)
  )
  set_target_properties(${T} PROPERTIES LINK_DEPENDS ${LDSCRIPT})
  add_custom_command(TARGET ${T} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${T}> ${B}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${T}> ${B}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${T}>
  )
endfunction()

add_gasmeter(pcb3_2)
add_gasmeter(pcb3_3)

# ---------- Nucleo-H563ZI 브레드보드 LoRa 벤치 (bench/nucleo_lora/main.c) ----------
# 보드 펌웨어와 따로 만듦: cmake --build build.nosync --target bench_lora.elf
# 넣기: ./tools/flash.sh build.nosync/bench_lora.elf   (Nucleo 원래 Zephyr 벤치는 build.nosync/backup/ 에 백업)
set(BENCH ${CMAKE_CURRENT_SOURCE_DIR}/bench/nucleo_lora)
add_executable(bench_lora.elf EXCLUDE_FROM_ALL
  ${BENCH}/main.c
  ${BENCH}/wioe5.c
  ${BENCH}/bme280.c
  ${BENCH}/sdp810.c
  ${BENCH}/ds18b20.c
  ${BENCH}/it.c
  Core/Src/hsc.c
  Core/Src/abp2.c
  Core/Src/report.c
  Core/Src/i2c_stm32.c
  Core/Src/i2c_timing.c
  Core/Src/lora_uart_stm32.c
  Core/Src/log.c
  Core/Src/syscalls.c
  Core/Src/stm32h5xx_hal_msp.c
  ${DEV}/Source/Templates/system_stm32h5xx.c
  ${DEV}/Source/Templates/gcc/startup_stm32h563xx.s
  ${HAL_SOURCES}
)
target_compile_definitions(bench_lora.elf PRIVATE STM32H563xx USE_HAL_DRIVER)
target_include_directories(bench_lora.elf PRIVATE
  ${BENCH}
  boards/pcb3_2               # stm32h5xx_hal_msp.c 의 로그 · LoRa 핀(Nucleo 와 같은 자리)
  Core/Inc
  ${DEV}/Include
  ${HAL}/Inc
  ${TP}/cmsis_core/Include
)
target_compile_options(bench_lora.elf PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-unused-parameter>
  $<$<CONFIG:Debug>:-Og -g3>
  $<$<CONFIG:Release>:-Os -g>
)
target_link_libraries(bench_lora.elf PRIVATE m)
target_link_options(bench_lora.elf PRIVATE
  -T${LDSCRIPT}
  -Wl,-Map=bench_lora.map,--cref
  -Wl,--print-memory-usage
  -u _printf_float
)
set_target_properties(bench_lora.elf PROPERTIES LINK_DEPENDS ${LDSCRIPT})
