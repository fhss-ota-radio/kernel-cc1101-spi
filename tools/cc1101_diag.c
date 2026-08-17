/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cc1101_diag.c - /dev/cc1101 진단 도구 (재빌드 없이 칩 내부 확인)
 *
 * [만든 이유, 2026-08-16]
 * 같은 하드웨어에서 유저공간 spidev 경로(SpidevTransport)는 99.5% 수신되는데
 * 커널 드라이버(/dev/cc1101) 경로만 수신측 GDO0/GDO2 인터럽트가 아예 0인
 * 상황을 만남. 안테나·배선·전원은 spidev 성공으로 배제됐으므로, 남은 가능성은
 *   (1) 초기화 때 레지스터가 칩에 제대로 안 써졌다 (SPI 신호 품질 문제)
 *   (2) 칩이 RX 상태로 안 들어가 있다
 * 두 가지. 이 도구는 그 둘을 직접 확인한다.
 *
 * 빌드 (라즈베리파이에서):
 *   gcc -O2 -o cc1101_diag cc1101_diag.c
 * 실행:
 *   sudo ./cc1101_diag            # 현재 상태만 확인
 *   sudo ./cc1101_diag --set-rx   # SET_RX 시킨 뒤 상태 확인
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cc1101_ioctl.h"

struct expected_reg {
	uint8_t addr;
	uint8_t value;
	const char *name;
};

/*
 * cc1101_core.c의 cc1101_default_regs[]에서 "이거 틀리면 수신이 안 되는"
 * 핵심 레지스터만 골라냄. 여기 값이 하나라도 안 맞으면 초기화 때 SPI 쓰기가
 * 깨졌다는 뜻이다.
 */
static const struct expected_reg kExpected[] = {
	{ 0x00, 0x07, "IOCFG2   (GDO2: CRC OK 수신 시 assert)" },
	{ 0x02, 0x06, "IOCFG0   (GDO0: 동기워드 송/수신 시 assert)" },
	{ 0x03, 0x47, "FIFOTHR" },
	{ 0x04, 0x2D, "SYNC1    (OTA 전용, 팀 공용 0xD3 아님)" },
	{ 0x05, 0xD4, "SYNC0    (OTA 전용, 팀 공용 0x91 아님)" },
	{ 0x06, 0x3D, "PKTLEN" },
	{ 0x07, 0x0C, "PKTCTRL1 (주소필터 꺼짐이어야 함)" },
	{ 0x08, 0x05, "PKTCTRL0 (가변길이 + CRC)" },
	{ 0x0A, 0x00, "CHANNR   (채널 0 = 433.92MHz)" },
	{ 0x0D, 0x10, "FREQ2    (433.92MHz)" },
	{ 0x0E, 0xB0, "FREQ1" },
	{ 0x0F, 0x71, "FREQ0" },
	{ 0x10, 0xCA, "MDMCFG4" },
	{ 0x11, 0x83, "MDMCFG3" },
	{ 0x12, 0x13, "MDMCFG2  (2-FSK, 16/16 sync)" },
	{ 0x17, 0x3F, "MCSM1    (TX/RX 종료 후 RX 유지)" },
	{ 0x18, 0x18, "MCSM0    (IDLE->RX/TX 자동 캘리브레이션)" },
};

static const char *marc_state_name(uint8_t s)
{
	switch (s) {
	case 0x00: return "SLEEP";
	case 0x01: return "IDLE";
	case 0x02: return "XOFF";
	case 0x03: return "VCOON_MC";
	case 0x04: return "REGON_MC";
	case 0x05: return "MANCAL";
	case 0x06: return "VCOON";
	case 0x07: return "REGON";
	case 0x08: return "STARTCAL";
	case 0x09: return "BWBOOST";
	case 0x0A: return "FS_LOCK";
	case 0x0B: return "IFADCON";
	case 0x0C: return "ENDCAL";
	case 0x0D: return "RX  <<< 수신 대기중 (정상)";
	case 0x0E: return "RX_END";
	case 0x0F: return "RX_RST";
	case 0x10: return "TXRX_SWITCH";
	case 0x11: return "RXFIFO_OVERFLOW";
	case 0x12: return "FSTXON";
	case 0x13: return "TX";
	case 0x14: return "TX_END";
	case 0x15: return "RXTX_SWITCH";
	case 0x16: return "TXFIFO_UNDERFLOW";
	default:   return "(알 수 없음)";
	}
}

int main(int argc, char *argv[])
{
	const char *path = "/dev/cc1101";
	int set_rx = 0;

	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--set-rx") == 0)
			set_rx = 1;
		else
			path = argv[i];
	}

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (set_rx) {
		printf("SET_RX 실행...\n");
		if (ioctl(fd, CC1101_IOC_SET_RX) < 0)
			perror("  SET_RX 실패");
		else
			printf("  SET_RX 성공\n");
		usleep(10000);
	}

	/* ---------- 1. 레지스터 검증 ---------- */
	printf("\n=== 레지스터 확인 (기대값 vs 실제) ===\n");
	int mismatch = 0, ioerr = 0;
	for (size_t i = 0; i < sizeof(kExpected) / sizeof(kExpected[0]); ++i) {
		struct cc1101_reg_io io;
		memset(&io, 0, sizeof(io));
		io.addr = kExpected[i].addr;

		if (ioctl(fd, CC1101_IOC_READ_REG, &io) < 0) {
			printf("  [IO오류] 0x%02X %-45s : 읽기 실패\n",
			       kExpected[i].addr, kExpected[i].name);
			++ioerr;
			continue;
		}

		const int ok = (io.value == kExpected[i].value);
		printf("  [%s] 0x%02X %-45s : 기대 0x%02X, 실제 0x%02X\n",
		       ok ? "OK  " : "틀림", kExpected[i].addr,
		       kExpected[i].name, kExpected[i].value, io.value);
		if (!ok)
			++mismatch;
	}

	/* ---------- 2. 칩 상태머신 ---------- */
	printf("\n=== 칩 상태 (MARCSTATE) ===\n");
	struct cc1101_status st;
	memset(&st, 0, sizeof(st));
	if (ioctl(fd, CC1101_IOC_GET_STATUS, &st) < 0) {
		perror("  GET_STATUS 실패");
	} else {
		printf("  marc_state = 0x%02X (%s)\n",
		       st.marc_state, marc_state_name(st.marc_state));
		printf("  rssi = %d dBm, lqi = %u, crc_ok = %u\n",
		       st.rssi_dbm, st.lqi, st.crc_ok);
	}

	/* ---------- 3. 결론 ---------- */
	printf("\n=== 판정 ===\n");
	if (ioerr)
		printf("  * READ_REG 자체가 실패함 -> ioctl/드라이버 문제부터 확인\n");
	if (mismatch)
		printf("  * 레지스터 %d개 불일치 -> 초기화 때 SPI 쓰기가 깨짐\n"
		       "    (배선 접촉 / SPI 클럭 속도 의심. 오버레이에\n"
		       "     dtoverlay=cc1101,speed=1000000 로 낮춰서 재시도)\n",
		       mismatch);
	if (!mismatch && !ioerr)
		printf("  * 레지스터는 전부 정상 -> 설정 문제 아님\n");

	printf("  * MARCSTATE가 0x0D(RX)가 아니면 칩이 수신 대기중이 아님.\n"
	       "    --set-rx 옵션으로 다시 돌려보고도 0x0D가 안 되면\n"
	       "    RX 진입 자체가 실패하는 것.\n");

	close(fd);
	return 0;
}
