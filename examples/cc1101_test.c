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
#define MAX_PACKET_LEN 60

static int open_dev(void)
{
	int fd = open(DEV_PATH, O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "%s open 실패: %s\n", DEV_PATH, strerror(errno));
		exit(1);
	}
	return fd;
}

static void cmd_tx(int fd, const char *arg, const char *msg)
{
	long value;
	char *end;
	unsigned char txbuf[MAX_PACKET_LEN]; // 메시지 담을 버퍼.
	size_t msg_len = strlen(msg);
	ssize_t n;
	value = strtol(arg, &end, 0); //주소
	if (arg == end || *end != '\0' || value < 0 || value > 255) {
		fprintf(stderr, "목적지 주소는 0~255 범위여야 합니다.\n");
		exit(1);
	}
	
	if (msg_len > sizeof(txbuf)-1) {
		fprintf(stderr, "메시지 길이가 너무 깁니다 (최대 %zu bytes)\n", sizeof(txbuf)-1);
		exit(1);
	}
	txbuf[0] = (unsigned char)value;
	if (msg_len > 0) {
		memcpy(&txbuf[1], msg, msg_len);
	}

	n=write(fd, txbuf, msg_len + 1);
	if (n < 0) {
		fprintf(stderr, "write 실패: %s\n", strerror(errno));
		exit(1);
	}

	printf("송신 완료 (%zd bytes): ", n);
	for (ssize_t i = 0; i < n; i++)
		printf("%02x ", txbuf[i]);				
	printf(" | \"%.*s\"\n", (int)(n-1), (char *)&txbuf[1]); //지정된 길이(int)n 만큼만 출력. %.*s


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
		if(n < 1){
			fprintf(stderr, "수신된 데이터가 없습니다.\n");
			continue;
		}
		printf("목적지 주소: %u\n", buf[0]);
		

		printf("메시지 (%zd bytes): \"%.*s\"\n",
		       n - 1, (int)(n - 1), (char *)&buf[1]);
	}
}
//상태 정보 받아오기
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
//주소 필터 비활성화
static void cmd_broadcast(int fd)
{
	__u8 mode = CC1101_ADDR_FILTER_DISABLE;

	if (ioctl(fd, CC1101_IOC_SET_ADDR_FILTER, &mode) < 0) {
		fprintf(stderr, "ioctl 실패: %s\n", strerror(errno));
		exit(1);
	}
	printf("주소 필터 비활성화 (1:N 브로드캐스트 모드)\n");
}
//채널 설정
static void cmd_channel(int fd, const char *arg)
{
	long value;
	char *end;
	__u8 channel;

	value = strtol(arg, &end, 0);
	if (arg == end || *end != '\0' || value < 0 || value > 255) {
		//잘못된 입력
		fprintf(stderr, "채널 번호는 0~255 범위여야 합니다.\n");
		exit(1);
	}
	channel = (__u8)value;

	if (ioctl(fd, CC1101_IOC_SET_CHANNEL, &channel) < 0) {
		fprintf(stderr, "ioctl 실패: %s\n", strerror(errno));
		exit(1);
	}
	printf("채널 설정 완료: %u\n", channel);
}
//주소 설정
static void cmd_addr(int fd, const char *arg)
{
	long value;
	char *end;
	__u8 addr;

	value = strtol(arg, &end, 0);
	if (arg == end || *end != '\0' || value < 0 || value > 255) {
		fprintf(stderr, "주소는 0~255 범위여야 합니다.\n");
		exit(1);
	}
	addr = (__u8)value;

	if (ioctl(fd, CC1101_IOC_SET_ADDR, &addr) < 0) {
		fprintf(stderr, "ioctl 실패: %s\n", strerror(errno));
		exit(1);
	}
	printf("주소 설정 완료: %u\n", addr);
}
static void cmd_set_addr_filter(int fd, const char *arg)
{
	long value;
	char *end;
	__u8 mode;

	value = strtol(arg, &end, 0);
	if (arg == end || *end != '\0' || value < 0 || value > 3) {
		fprintf(stderr, "주소 필터 모드는 0~3 범위여야 합니다.\n");
		exit(1);
	}
	mode = (__u8)value;

	if (ioctl(fd, CC1101_IOC_SET_ADDR_FILTER, &mode) < 0) {
		fprintf(stderr, "ioctl 실패: %s\n", strerror(errno));
		exit(1);
	}
	printf("주소 필터 모드 설정 완료: %u\n", mode);
}

int main(int argc, char **argv)
{
	int fd;

	if (argc < 2) {
		fprintf(stderr, "사용법: %s {tx <msg>|rx|status|broadcast|channel <0-255>|addr <0-255>|set_addr_filter <0-3>}\n", argv[0]);
		return 1;
	}

	fd = open_dev();

	if (!strcmp(argv[1], "tx") && argc >= 4)
		cmd_tx(fd, argv[2],argv[3]);
	else if (!strcmp(argv[1], "rx"))
		cmd_rx(fd);
	else if (!strcmp(argv[1], "status"))
		cmd_status(fd);
	else if (!strcmp(argv[1], "broadcast"))
		cmd_broadcast(fd);
	else if (!strcmp(argv[1], "channel") && argc >= 3)
		cmd_channel(fd, argv[2]);
	else if (!strcmp(argv[1], "addr") && argc >= 3)
		cmd_addr(fd, argv[2]);
	else if (!strcmp(argv[1], "set_addr_filter") && argc >= 3)
		cmd_set_addr_filter(fd, argv[2]);
	else
		fprintf(stderr, "알 수 없는 명령: %s\n", argv[1]);

	close(fd);
	return 0;
}
