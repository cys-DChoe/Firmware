/* 이더넷(DHCP) + MQTT 로 측정 줄을 PC 브로커에 보냄 */
#ifndef BENCH_NET_H_
#define BENCH_NET_H_

/* DHCP 시작 + 전송 스레드 시작. main() 처음에 한 번 부름 */
void bench_net_start(void);

/* 시리얼에 찍는 줄과 같은 글을 MQTT 로 보낼 줄 대기열에 넣음 (줄바꿈 없이) */
void bench_net_send(const char *line);

#endif
