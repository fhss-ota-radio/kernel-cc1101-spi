// SPDX-License-Identifier: GPL-2.0
/*
 * Raspberry Pi 두 대에 각각 연결된 CC1101의 링크를 확인하는 테스트 앱.
 *
 * 1단계 fixed: FHSS를 사용하지 않고 채널 0에서 PING/PONG을 확인한다.
 * 2단계 fhss : 같은 설정을 넣고 드라이버의 자동 호핑과 동기화를 확인한다.
 *
 * 한쪽은 master, 다른 쪽은 slave로 실행한다. master가 PING을 보내고
 * slave가 PONG으로 답하므로 TX와 RX를 한 번에 검증할 수 있다.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../cc1101_ioctl.h"

#define TEST_PACKET_SIZE 10u
#define TEST_PACKET_VERSION 1u
#define TEST_KIND_PING 1u
#define TEST_KIND_PONG 2u

struct app_config {
	const char *device;
	int use_fhss;
	int is_master;
	uint8_t fixed_channel;
	uint8_t channel_count;
	uint32_t seed;
	uint32_t generation;
	uint32_t interval_ms;
};

struct counters {
	uint32_t ping_tx;
	uint32_t ping_rx;
	uint32_t pong_tx;
	uint32_t pong_rx;
	uint32_t invalid_rx;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)value;
	dst[1] = (uint8_t)(value >> 8);
	dst[2] = (uint8_t)(value >> 16);
	dst[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32_le(const uint8_t *src)
{
	return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
	       ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void make_packet(uint8_t *packet, uint8_t kind, uint32_t sequence)
{
	packet[0] = 'R';
	packet[1] = 'P';
	packet[2] = 'I';
	packet[3] = 'T';
	packet[4] = TEST_PACKET_VERSION;
	packet[5] = kind;
	put_u32_le(&packet[6], sequence);
}

static int parse_packet(const uint8_t *packet, size_t length,
			uint8_t *kind, uint32_t *sequence)
{
	if (length != TEST_PACKET_SIZE || packet[0] != 'R' ||
	    packet[1] != 'P' || packet[2] != 'I' || packet[3] != 'T' ||
	    packet[4] != TEST_PACKET_VERSION)
		return -1;
	if (packet[5] != TEST_KIND_PING && packet[5] != TEST_KIND_PONG)
		return -1;
	*kind = packet[5];
	*sequence = get_u32_le(&packet[6]);
	return 0;
}

static int parse_u32(const char *text, uint32_t maximum, uint32_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno || text == end || *end != '\0' || parsed > maximum)
		return -1;
	*value = (uint32_t)parsed;
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"사용법: %s {fixed|fhss} {master|slave} [옵션]\n"
		"\n"
		"옵션:\n"
		"  --device PATH       장치 경로 (기본 /dev/cc1101)\n"
		"  --channel N         fixed 채널 (기본 0)\n"
		"  --channels N        FHSS 채널 수 (기본 4, 채널 1~4)\n"
		"  --seed N            공용 seed (기본 0x46485353)\n"
		"  --generation N      FHSS generation (기본 1)\n"
		"  --interval MS       PING 간격 (기본 1000ms)\n"
		"\n"
		"예시:\n"
		"  Pi A: sudo %s fixed master\n"
		"  Pi B: sudo %s fixed slave\n"
		"  Pi B: sudo %s fhss slave\n"
		"  Pi A: sudo %s fhss master\n",
		program, program, program, program, program);
}

static int parse_arguments(int argc, char **argv, struct app_config *config)
{
	int i;

	if (argc < 3)
		return -1;
	if (!strcmp(argv[1], "fixed"))
		config->use_fhss = 0;
	else if (!strcmp(argv[1], "fhss"))
		config->use_fhss = 1;
	else
		return -1;

	if (!strcmp(argv[2], "master"))
		config->is_master = 1;
	else if (!strcmp(argv[2], "slave"))
		config->is_master = 0;
	else
		return -1;

	for (i = 3; i < argc; i++) {
		const char *name = argv[i];
		uint32_t value;

		if (!strcmp(name, "--device") && i + 1 < argc) {
			config->device = argv[++i];
			continue;
		}
		if (i + 1 >= argc)
			return -1;
		if (parse_u32(argv[++i], UINT32_MAX, &value))
			return -1;
		if (!strcmp(name, "--channel") && value <= UINT8_MAX)
			config->fixed_channel = (uint8_t)value;
		else if (!strcmp(name, "--channels") && value >= 2u && value <= 4u)
			config->channel_count = (uint8_t)value;
		else if (!strcmp(name, "--seed"))
			config->seed = value;
		else if (!strcmp(name, "--generation"))
			config->generation = value;
		else if (!strcmp(name, "--interval") && value >= 200u)
			config->interval_ms = value;
		else
			return -1;
	}
	return 0;
}

static int set_fixed_mode(int fd, uint8_t channel)
{
	uint8_t filter = CC1101_ADDR_FILTER_DISABLE;

	/* 이전 실행에서 FHSS가 남았더라도 STOP으로 타이머를 먼저 끝낸다.
	 * 설정된 FHSS가 없을 때의 오류는 fixed 테스트 진행에 영향이 없다. */
	(void)ioctl(fd, CC1101_IOC_FHSS_STOP);
	if (ioctl(fd, CC1101_IOC_SET_ADDR_FILTER, &filter) < 0 ||
	    ioctl(fd, CC1101_IOC_SET_CHANNEL, &channel) < 0 ||
	    ioctl(fd, CC1101_IOC_FLUSH_RX) < 0) {
		fprintf(stderr, "고정 채널 설정 실패: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int set_fhss_mode(int fd, const struct app_config *app)
{
	struct cc1101_fhss_config config;
	uint8_t role = app->is_master ? CC1101_FHSS_ROLE_MASTER :
		CC1101_FHSS_ROLE_SLAVE;

	memset(&config, 0, sizeof(config));
	config.version = CC1101_FHSS_VERSION;
	config.size = sizeof(config);
	config.generation = app->generation;
	config.algorithm_id = CC1101_FHSS_ALGORITHM_SEEDED_PERMUTATION;

	/* ESP32 develop의 CC1101 설정과 같은 물리 계층 값이다. */
	config.rf.base_freq_hz = 433919830u;
	config.rf.channel_spacing_hz = 199951u;
	config.rf.sync_word = 0xD391u;
	config.rf.mdmcfg4 = 0xCAu;
	config.rf.mdmcfg3 = 0x83u;
	config.rf.pktctrl1 = 0x04u;
	config.rf.pktctrl0 = 0x05u;

	config.hop.seed = app->seed;
	config.hop.slot_duration_us = 300000u;
	config.hop.channel_switch_guard_us = 5000u;
	config.hop.channel_count = app->channel_count;
	config.hop.first_channel = 1u;
	config.hop.rendezvous_channel = 1u;
	config.hop.reserved_channel = 0u;
	config.hop.algorithm_version = CC1101_FHSS_ALGORITHM_VERSION;
	config.hop.channel_profile_id = 0u;

	(void)ioctl(fd, CC1101_IOC_FHSS_STOP);
	if (ioctl(fd, CC1101_IOC_FHSS_SET_CONFIG, &config) < 0) {
		fprintf(stderr, "FHSS 설정 실패: %s (드라이버 UAPI v2 확인)\n",
			strerror(errno));
		return -1;
	}
	if (ioctl(fd, CC1101_IOC_FHSS_START, &role) < 0) {
		fprintf(stderr, "FHSS 시작 실패: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int send_test_packet(int fd, uint8_t kind, uint32_t sequence)
{
	uint8_t packet[TEST_PACKET_SIZE];
	ssize_t written;

	make_packet(packet, kind, sequence);
	written = write(fd, packet, sizeof(packet));
	if (written == (ssize_t)sizeof(packet))
		return 0;
	if (written < 0 && (errno == EBUSY || errno == EAGAIN))
		return 1;
	fprintf(stderr, "write 실패: %s\n",
		written < 0 ? strerror(errno) : "짧은 쓰기");
	return -1;
}

static void print_fhss_status(int fd)
{
	struct cc1101_fhss_status status;

	memset(&status, 0, sizeof(status));
	if (ioctl(fd, CC1101_IOC_FHSS_GET_STATUS, &status) < 0) {
		fprintf(stderr, "FHSS 상태 조회 실패: %s\n", strerror(errno));
		return;
	}
	printf("[FHSS] enabled=%u synchronized=%u role=%u channel=%u "
	       "slot=%llu sync_rx=%u misses=%u error=%d\n",
	       status.enabled, status.synchronized, status.role,
	       status.current_channel, (unsigned long long)status.current_slot,
	       status.sync_packets, status.sync_misses, status.last_error);
}

static int run_link_test(int fd, const struct app_config *app)
{
	struct counters stats = {0};
	uint64_t next_ping_ms = monotonic_ms() + 500u;
	uint64_t next_status_ms = monotonic_ms();
	uint32_t next_sequence = 1u;

	while (!stop_requested) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint64_t now_ms;
		int ready = poll(&pfd, 1, 100);

		if (ready < 0 && errno != EINTR) {
			fprintf(stderr, "poll 실패: %s\n", strerror(errno));
			return -1;
		}
		if (ready > 0 && (pfd.revents & POLLIN)) {
			uint8_t packet[64];
			uint8_t kind;
			uint32_t sequence;
			ssize_t length = read(fd, packet, sizeof(packet));

			if (length > 0 && !parse_packet(packet, (size_t)length,
						    &kind, &sequence)) {
				if (kind == TEST_KIND_PING) {
					stats.ping_rx++;
					printf("[RX] PING seq=%u\n", sequence);
					if (!app->is_master &&
					    send_test_packet(fd, TEST_KIND_PONG, sequence) == 0) {
						stats.pong_tx++;
						printf("[TX] PONG seq=%u\n", sequence);
					}
				} else {
					stats.pong_rx++;
					printf("[RX] PONG seq=%u (왕복 성공)\n", sequence);
				}
			} else if (length > 0) {
				stats.invalid_rx++;
				printf("[RX] 다른 패킷 %zd bytes (무시)\n", length);
			}
		}

		now_ms = monotonic_ms();
		if (app->is_master && now_ms >= next_ping_ms) {
			int synchronized = 1;

			if (app->use_fhss) {
				struct cc1101_fhss_status status;
				memset(&status, 0, sizeof(status));
				synchronized = ioctl(fd, CC1101_IOC_FHSS_GET_STATUS,
						     &status) == 0 && status.synchronized;
			}
			if (synchronized) {
				int result = send_test_packet(fd, TEST_KIND_PING,
							      next_sequence);
				if (result == 0) {
					printf("[TX] PING seq=%u\n", next_sequence);
					stats.ping_tx++;
					next_sequence++;
				}
			}
			next_ping_ms = now_ms + app->interval_ms;
		}
		if (now_ms >= next_status_ms) {
			if (app->use_fhss)
				print_fhss_status(fd);
			printf("[통계] ping tx/rx=%u/%u, pong tx/rx=%u/%u, other=%u\n",
			       stats.ping_tx, stats.ping_rx, stats.pong_tx,
			       stats.pong_rx, stats.invalid_rx);
			next_status_ms = now_ms + 2000u;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct app_config config = {
		.device = "/dev/cc1101",
		.fixed_channel = 0u,
		.channel_count = 4u,
		.seed = CC1101_FHSS_DEFAULT_SEED,
		.generation = 1u,
		.interval_ms = 1000u,
	};
	int fd;
	int result;

	if (parse_arguments(argc, argv, &config)) {
		usage(argv[0]);
		return 2;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	fd = open(config.device, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "%s 열기 실패: %s\n", config.device,
			strerror(errno));
		return 1;
	}

	printf("모드=%s 역할=%s 장치=%s\n",
	       config.use_fhss ? "FHSS" : "고정 채널",
	       config.is_master ? "MASTER(PING)" : "SLAVE(PONG)",
	       config.device);
	if (config.use_fhss) {
		printf("FHSS: channels=1~%u seed=0x%08x generation=%u\n",
		       config.channel_count, config.seed, config.generation);
		result = set_fhss_mode(fd, &config);
	} else {
		printf("고정 채널: %u\n", config.fixed_channel);
		result = set_fixed_mode(fd, config.fixed_channel);
	}
	if (!result)
		result = run_link_test(fd, &config);

	if (config.use_fhss && ioctl(fd, CC1101_IOC_FHSS_STOP) < 0)
		fprintf(stderr, "FHSS 종료 실패: %s\n", strerror(errno));
	close(fd);
	printf("테스트 종료\n");
	return result ? 1 : 0;
}
