/* 판 확인 (설계서 판 구분 탭 4번, 11/25): 켤 때 PA9 를 풀업 입력으로 한 번 읽어 빌드한 판과 맞대 봄.
 *   PCB3_3 = PA9 가 판에서 GND(읽으면 0) · PCB3_2 = 빈 핀(풀업으로 1)
 * 핀 읽기는 main.c(board_pa9_low), 판정 · 로그 글은 여기 — 맥 시험(test_app.c)이 4가지 경우를 봄. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* 반환 = 맞게 구움. msg 에 로그 한 줄(끝 줄바꿈 없음) */
static inline bool board_check(bool build_pa9_gnd, bool pa9_low, const char *build_name, char *msg, size_t n)
{
    const char *seen = pa9_low ? "PCB3_3" : "PCB3_2";
    if (pa9_low == build_pa9_gnd) {
        snprintf(msg, n, "판 확인: PA9 = %d (%s 맞음)", pa9_low ? 0 : 1, build_name);
        return true;
    }
    snprintf(msg, n, "판 확인: 빌드는 %s 인데 PA9 = %d → %s 판. 펌웨어 잘못 구움 (빨강 4번)", build_name,
             pa9_low ? 0 : 1, seen);
    return false;
}
