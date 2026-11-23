/*
 * 이더넷(DHCP) + MQTT 전송
 *
 * - 랜선이 꽂히면 DHCP 로 주소를 받고, CONFIG_BENCH_MQTT_BROKER 의 브로커에 붙음.
 * - main 이 시리얼에 찍는 줄을 그대로 받아 대기열에 쌓고, 붙어 있으면 오래된 것부터 보냄.
 *   끊긴 동안은 CONFIG_BENCH_MQTT_QUEUE 줄까지 쌓아 두고, 넘치면 가장 오래된 줄을 버림.
 * - 끊기면 5초마다 다시 붙음.
 *
 * 토픽 (CONFIG_BENCH_MQTT_TOPIC = T):
 *   T/line    측정 줄과 안내 줄 (시리얼 출력과 같은 글)
 *   T/status  "online <보드 IP>" (retain). 보드가 끊기면 브로커가 "offline" 으로 바꿈 (will)
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>

#include "bench_net.h"

#define LINE_MAX     320 /* 측정 줄 최대 길이 (센서 5개가 다 붙으면 약 230 바이트). main.c 의 say() 와 같게 */
#define RETRY_MS     5000
#define CONNACK_WAIT 5000

#define TOPIC_LINE   CONFIG_BENCH_MQTT_TOPIC "/line"
#define TOPIC_STATUS CONFIG_BENCH_MQTT_TOPIC "/status"

K_MSGQ_DEFINE(line_q, LINE_MAX, CONFIG_BENCH_MQTT_QUEUE, 4);

static K_SEM_DEFINE(got_address, 0, 1);
static struct net_mgmt_event_callback mgmt_cb;
static char my_ip[NET_IPV4_ADDR_LEN] = "?";

static struct mqtt_client client;
static struct net_sockaddr_storage broker;
static struct mqtt_utf8 user, pass;
static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_msg;
static uint8_t rx_buf[256];
static uint8_t tx_buf[LINE_MAX + 128];
static volatile bool connected;
static uint16_t next_msg_id = 1;

void bench_net_send(const char *line)
{
	char slot[LINE_MAX];

	strncpy(slot, line, sizeof(slot) - 1);
	slot[sizeof(slot) - 1] = '\0';
	while (k_msgq_put(&line_q, slot, K_NO_WAIT) != 0) {
		char junk[LINE_MAX];

		(void)k_msgq_get(&line_q, junk, K_NO_WAIT); /* 꽉 찼으면 가장 오래된 줄 버림 */
	}
}

static void on_net_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	struct net_if_ipv4 *ipv4;

	ARG_UNUSED(cb);
	if (event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}
	ipv4 = iface->config.ip.ipv4;
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *a = &ipv4->unicast[i].ipv4;
		char gw[NET_IPV4_ADDR_LEN];

		if (!a->is_used || a->addr_type != NET_ADDR_DHCP) {
			continue;
		}
		net_addr_ntop(NET_AF_INET, &a->address.in_addr, my_ip, sizeof(my_ip));
		net_addr_ntop(NET_AF_INET, &ipv4->gw, gw, sizeof(gw));
		printk("[네트] DHCP 주소 %s (게이트웨이 %s). 브로커 %s:%d 로 보냄\n", my_ip, gw,
		       CONFIG_BENCH_MQTT_BROKER, CONFIG_BENCH_MQTT_PORT);
		k_sem_give(&got_address);
		return;
	}
}

static void evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			connected = true;
		} else {
			printk("[네트] 브로커가 연결 거부 (CONNACK %d): 계정 확인 (secrets.conf)\n",
			       evt->result);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		connected = false;
		break;
	default:
		break;
	}
}

static int client_setup(void)
{
	struct net_sockaddr_in *b = (struct net_sockaddr_in *)&broker;

	mqtt_client_init(&client);

	b->sin_family = NET_AF_INET;
	b->sin_port = net_htons(CONFIG_BENCH_MQTT_PORT);
	if (net_addr_pton(NET_AF_INET, CONFIG_BENCH_MQTT_BROKER, &b->sin_addr) != 0) {
		printk("[네트] 브로커 주소 형식 오류: %s\n", CONFIG_BENCH_MQTT_BROKER);
		return -EINVAL;
	}

	user.utf8 = (const uint8_t *)CONFIG_BENCH_MQTT_USER;
	user.size = strlen(CONFIG_BENCH_MQTT_USER);
	pass.utf8 = (const uint8_t *)CONFIG_BENCH_MQTT_PASSWORD;
	pass.size = strlen(CONFIG_BENCH_MQTT_PASSWORD);

	will_topic.topic.utf8 = (const uint8_t *)TOPIC_STATUS;
	will_topic.topic.size = strlen(TOPIC_STATUS);
	will_topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	will_msg.utf8 = (const uint8_t *)"offline";
	will_msg.size = strlen("offline");

	client.broker = &broker;
	client.evt_cb = evt_handler;
	client.client_id.utf8 = (const uint8_t *)CONFIG_BENCH_MQTT_CLIENT_ID;
	client.client_id.size = strlen(CONFIG_BENCH_MQTT_CLIENT_ID);
	client.user_name = user.size ? &user : NULL;
	client.password = pass.size ? &pass : NULL;
	client.will_topic = &will_topic;
	client.will_message = &will_msg;
	client.will_retain = 1;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;
	return 0;
}

/* 소켓에 들어온 게 있으면 처리. 0 = 정상(타임아웃 포함), 음수 = 끊김 */
static int pump_input(int timeout_ms)
{
	struct zsock_pollfd fds = {
		.fd = client.transport.tcp.sock,
		.events = ZSOCK_POLLIN,
	};
	int ret = zsock_poll(&fds, 1, timeout_ms);

	if (ret < 0) {
		return -errno;
	}
	if (ret == 0) {
		return 0;
	}
	if (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
		return -ENOTCONN;
	}
	return mqtt_input(&client);
}

static int publish(const char *topic, const char *msg, bool retain)
{
	struct mqtt_publish_param p = {0};

	p.message.topic.topic.utf8 = (const uint8_t *)topic;
	p.message.topic.topic.size = strlen(topic);
	p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	p.message.payload.data = (uint8_t *)msg;
	p.message.payload.len = strlen(msg);
	p.message_id = next_msg_id++;
	p.retain_flag = retain ? 1 : 0;
	return mqtt_publish(&client, &p);
}

static int try_connect(void)
{
	int64_t until;
	int ret = client_setup();

	if (ret != 0) {
		return ret;
	}
	ret = mqtt_connect(&client);
	if (ret != 0) {
		return ret; /* 브로커에 TCP 연결 실패 */
	}
	until = k_uptime_get() + CONNACK_WAIT;
	while (!connected && k_uptime_get() < until) {
		if (pump_input(500) < 0) {
			break;
		}
	}
	if (!connected) {
		mqtt_abort(&client);
		return -ETIMEDOUT;
	}

	char st[48];

	snprintk(st, sizeof(st), "online %s", my_ip);
	(void)publish(TOPIC_STATUS, st, true);
	printk("[네트] MQTT 연결됨 (%s:%d, 토픽 %s)\n", CONFIG_BENCH_MQTT_BROKER,
	       CONFIG_BENCH_MQTT_PORT, TOPIC_LINE);
	return 0;
}

static void drop_connection(int why)
{
	printk("[네트] MQTT 끊김 (%d). 줄은 쌓아 두고 다시 붙음\n", why);
	mqtt_abort(&client);
	connected = false;
}

static void net_thread(void *a, void *b, void *c)
{
	static char cur[LINE_MAX];
	int64_t next_retry = 0;
	int last_err = 1;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* 주소를 못 받는 동안 10초마다 링크 상태를 찍어 원인을 가름 */
	for (int waited = 10; k_sem_take(&got_address, K_SECONDS(10)) != 0; waited += 10) {
		struct net_if *iface = net_if_get_default();
		struct net_linkaddr *ll = net_if_get_link_addr(iface);

		printk("[네트] %d초째 주소 없음: 랜선 링크 %s, MAC %02X:%02X:%02X:%02X:%02X:%02X%s\n",
		       waited, net_if_is_carrier_ok(iface) ? "붙음" : "안 붙음 (선·포트 확인)",
		       ll->addr[0], ll->addr[1], ll->addr[2], ll->addr[3], ll->addr[4], ll->addr[5],
		       net_if_is_carrier_ok(iface) ? ". 그 포트가 DHCP 를 안 주거나 응답이 늦음" : "");
	}

	for (;;) {
		int ret;

		if (!connected && k_uptime_get() >= next_retry) {
			ret = try_connect();
			if (ret != 0 && ret != last_err) {
				printk("[네트] 브로커 %s:%d 연결 실패 (%d). 5초마다 다시 시도\n",
				       CONFIG_BENCH_MQTT_BROKER, CONFIG_BENCH_MQTT_PORT, ret);
			}
			last_err = ret;
			next_retry = k_uptime_get() + RETRY_MS;
		}

		if (connected) {
			while (k_msgq_peek(&line_q, cur) == 0) {
				ret = publish(TOPIC_LINE, cur, false);
				if (ret != 0) {
					drop_connection(ret);
					break;
				}
				(void)k_msgq_get(&line_q, cur, K_NO_WAIT);
			}
		}

		if (connected) {
			ret = pump_input(100);
			if (ret == 0) {
				ret = mqtt_live(&client);
				if (ret == -EAGAIN) {
					ret = 0;
				}
			}
			if (ret != 0) {
				drop_connection(ret);
			}
		} else {
			k_msleep(100);
		}
	}
}

K_THREAD_STACK_DEFINE(net_stack, 4096);
static struct k_thread net_thread_data;

void bench_net_start(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		printk("[네트] 네트워크 인터페이스 없음: MQTT 안 보냄\n");
		return;
	}
	net_mgmt_init_event_callback(&mgmt_cb, on_net_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&mgmt_cb);
	net_dhcpv4_start(iface);
	printk("[네트] 랜선 연결 대기 + DHCP 요청 중 (주소 받으면 MQTT 로 보냄)\n");

	k_thread_create(&net_thread_data, net_stack, K_THREAD_STACK_SIZEOF(net_stack), net_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&net_thread_data, "bench_net");
}
