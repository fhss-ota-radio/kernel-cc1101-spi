// SPDX-License-Identifier: GPL-2.0
/*
 * cc1101_test.c - /dev/cc1101 사용 예제 겸 간단한 송수신 테스트 도구
 *
 * 빌드: gcc -o cc1101_test cc1101_test.c
 * 사용:
 *   ./cc1101_test tx "hello"          # 문자열 1패킷 송신
 *   ./cc1101_test rx                  # 수신 대기 후 패킷 덤프 (Ctrl-C 종료)
 *   ./cc1101_test status              # RSSI/LQI/상태 조회
 *   ./cc1101_test broadcast           # 주소 필터 비활성화 (1:N 브로드캐스트 모드)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "../cc1101_ioctl.h"

#define DEV_PATH "/dev/cc1101"

static int open_dev(void)
{
	int fd = open(DEV_PATH, O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "%s open 실패: %s\n", DEV_PATH, strerror(errno));
		exit(1);
	}
	return fd;
}

static void cmd_tx(int fd, const char *msg)
{
	ssize_t n = write(fd, msg, strlen(msg));

	if (n < 0) {
		fprintf(stderr, "write 실패: %s\n", strerror(errno));
		exit(1);
	}
	printf("송신 완료: %zd bytes\n", n);
}

static void cmd_rx(int fd)
{
	unsigned char buf[256];

	printf("수신 대기 중... (Ctrl-C로 종료)\n");
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0) {
			fprintf(stderr, "read 실패: %s\n", strerror(errno));
			exit(1);
		}

		printf("수신 (%zd bytes): ", n);
		for (ssize_t i = 0; i < n; i++)
			printf("%02x ", buf[i]);
		printf(" | \"%.*s\"\n", (int)n, buf);
	}
}

static void cmd_status(int fd)
{
	struct cc1101_status st;

	if (ioctl(fd, CC1101_IOC_GET_STATUS, &st) < 0) {
		fprintf(stderr, "ioctl 실패: %s\n", strerror(errno));
		exit(1);
	}

	printf("RSSI: %d dBm\n", st.rssi_dbm);
	printf("LQI : %u\n", st.lqi);
	printf("CRC : %s\n", st.crc_ok ? "OK" : "FAIL");
	printf("MARCSTATE: 0x%02x\n", st.marc_state);
}

static void cmd_broadcast(int fd)
{
	__u8 mode = CC1101_ADDR_FILTER_DISABLE;

	if (ioctl(fd, CC1101_IOC_SET_ADDR_FILTER, &mode) < 0) {
		fprintf(stderr, "ioctl 실패: %s\n", strerror(errno));
		exit(1);
	}
	printf("주소 필터 비활성화 (1:N 브로드캐스트 모드)\n");
}

int main(int argc, char **argv)
{
	int fd;

	if (argc < 2) {
		fprintf(stderr, "사용법: %s {tx <msg>|rx|status|broadcast}\n", argv[0]);
		return 1;
	}

	fd = open_dev();

	if (!strcmp(argv[1], "tx") && argc >= 3)
		cmd_tx(fd, argv[2]);
	else if (!strcmp(argv[1], "rx"))
		cmd_rx(fd);
	else if (!strcmp(argv[1], "status"))
		cmd_status(fd);
	else if (!strcmp(argv[1], "broadcast"))
		cmd_broadcast(fd);
	else
		fprintf(stderr, "알 수 없는 명령: %s\n", argv[1]);

	close(fd);
	return 0;
}
