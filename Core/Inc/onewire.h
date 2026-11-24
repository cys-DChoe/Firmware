/* 1-Wire 버스 틀 — DS18B20 드라이버는 이것만 보고, 실제 핀은 보드(onewire_stm32.c) 또는 맥 시험(가짜 버스)이 채움
 * 시간은 DS18B20 데이터시트(Dallas 050400) 그림 "1-Wire time slots" · "Initialization timing" 기준:
 *   리셋 480 µs 낮춤 → 놓고 70 µs 뒤 읽어 낮으면 센서 있음 → 410 µs 기다림
 *   쓰기 1: 5 µs 낮춤 → 60 µs 놓음 / 쓰기 0: 60 µs 낮춤 → 5 µs 놓음 (한 칸 최소 60 µs)
 *   읽기: 3 µs 낮춤 → 놓고 9 µs 뒤 읽음 → 55 µs 기다림 (낮춘 뒤 15 µs 안에 읽어야 함)
 * 비트 하나는 인터럽트 없이 끝나야 해서 보드 구현은 그 동안만 인터럽트를 막음.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct onewire {
    bool (*reset)(void *ctx);                   /* true = 센서가 존재 펄스로 답함 */
    void (*write_bit)(void *ctx, bool bit);
    bool (*read_bit)(void *ctx);
    void *ctx;
} onewire_t;

void onewire_write_byte(const onewire_t *ow, uint8_t v);   /* 작은 비트 먼저 */
uint8_t onewire_read_byte(const onewire_t *ow);
uint8_t onewire_crc8(const uint8_t *p, int n);             /* 다항식 x⁸+x⁵+x⁴+1 (거꾸로 0x8C) */
