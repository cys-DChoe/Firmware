/* newlib 이 찾는 최소 시스템 호출 — 파일 시스템이 없으니 대부분 "안 됨"으로 답함.
 * _write 는 log.c 에 있음(printf → USART3). */
#include <errno.h>
#include <sys/stat.h>

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int ptr, int dir) { (void)fd; (void)ptr; (void)dir; return 0; }
int _read(int fd, char *ptr, int len) { (void)fd; (void)ptr; (void)len; errno = EAGAIN; return -1; }

/* printf 가 쓰는 힙 — 링커 스크립트의 'end' 부터 스택(_Min_Stack_Size) 앞까지 */
#include <stddef.h>
#include <stdint.h>
extern uint8_t end;
extern uint8_t _estack;
extern uint32_t _Min_Stack_Size;

void *_sbrk(ptrdiff_t incr)
{
    static uint8_t *heap_end;
    const uint8_t *limit = &_estack - (uint32_t)&_Min_Stack_Size;
    if (heap_end == NULL) {
        heap_end = &end;
    }
    if (heap_end + incr > limit) {
        errno = ENOMEM;
        return (void *)-1;
    }
    uint8_t *prev = heap_end;
    heap_end += incr;
    return prev;
}

/* 소수 printf(-u _printf_float)가 abort 를 끌어와서 필요한 빈 함수들 — 실제로 불리면 멈춤 */
void _exit(int status)
{
    (void)status;
    while (1) {
    }
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    return -1;
}

int _getpid(void)
{
    return 1;
}
