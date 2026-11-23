# 브레드보드 센서 확인용 (NUCLEO-H563ZI + HSC + ABP2)
# 센서 드라이버는 이 저장소(FirmWare/Core)의 것을 그대로 가져다 씀.
cmake_minimum_required(VERSION 3.20.0)
# MQTT 비밀번호는 secrets.conf 에 따로 둠 (공유 금지)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/secrets.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/secrets.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensor_bench)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../../Core)
target_include_directories(app PRIVATE ${FW}/Inc)
target_sources(app PRIVATE
  src/main.c
  src/bench_net.c
  src/sdp810.c
  src/bme280.c
  src/ds18b20.c
  ${FW}/Src/hsc.c
  ${FW}/Src/abp2.c)
