// SPDX-License-Identifier: GPL-2.0
/*
 * cc1101_core.c - SPI 레지스터/스트로브 접근 및 칩 초기화 로직
 *
 * 이 파일의 모든 함수는 호출자가 필요 시 cc->lock을 잡은 상태에서 호출한다
 * (여러 SPI 트랜잭션을 원자적으로 묶어야 하는 상위 로직은 cc1101_main.c에서 처리).
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/bitops.h>

#include "cc1101.h"

int cc1101_read_reg(struct cc1101 *cc, u8 addr, u8 *val)
{
	u8 cmd = (addr & 0x3F) | CC1101_READ_SINGLE;

	return spi_write_then_read(cc->spi, &cmd, 1, val, 1);
}

int cc1101_write_reg(struct cc1101 *cc, u8 addr, u8 val)
{
	u8 buf[2] = { (addr & 0x3F) | CC1101_WRITE_SINGLE, val };

	return spi_write(cc->spi, buf, sizeof(buf));
}

/* 상태 레지스터(0x30-0x3D)는 burst 비트를 세우지 않으면 스트로브 명령으로 해석된다 */
int cc1101_read_status_reg(struct cc1101 *cc, u8 addr, u8 *val)
{
	u8 cmd = (addr & 0x3F) | CC1101_READ_BURST;

	return spi_write_then_read(cc->spi, &cmd, 1, val, 1);
}

int cc1101_read_burst(struct cc1101 *cc, u8 addr, u8 *buf, size_t len)
{
	u8 cmd = (addr & 0x3F) | CC1101_READ_BURST;
	struct spi_transfer t[2] = {
		{ .tx_buf = &cmd, .len = 1, },
		{ .rx_buf = buf,  .len = len, },
	};
	struct spi_message m;

	spi_message_init(&m);
	spi_message_add_tail(&t[0], &m);
	spi_message_add_tail(&t[1], &m);

	return spi_sync(cc->spi, &m);
}

int cc1101_write_burst(struct cc1101 *cc, u8 addr, const u8 *buf, size_t len)
{
	u8 cmd = (addr & 0x3F) | CC1101_WRITE_BURST;
	struct spi_transfer t[2] = {
		{ .tx_buf = &cmd, .len = 1, },
		{ .tx_buf = buf,  .len = len, },
	};
	struct spi_message m;

	spi_message_init(&m);
	spi_message_add_tail(&t[0], &m);
	spi_message_add_tail(&t[1], &m);

	return spi_sync(cc->spi, &m);
}

int cc1101_strobe(struct cc1101 *cc, u8 strobe)
{
	return spi_write(cc->spi, &strobe, 1);
}

/*
 * 리셋 시퀀스 (datasheet 19.1.2 Reset). 전원 인가 직후가 아닌 런타임 재설정
 * 상황을 가정해 SRES 스트로브만 사용하는 단순화된 절차를 쓴다. 칩은 리셋
 * 완료까지 최대 수백 us가 걸리므로 여유있게 대기한다.
 */
int cc1101_hw_reset(struct cc1101 *cc)
{
	u8 partnum, version;
	int ret;

	ret = cc1101_strobe(cc, CC1101_SRES);
	if (ret)
		return ret;

	usleep_range(200, 300);

	ret = cc1101_read_status_reg(cc, CC1101_PARTNUM, &partnum);
	if (ret)
		return ret;
	ret = cc1101_read_status_reg(cc, CC1101_VERSION, &version);
	if (ret)
		return ret;

	/* SPI 배선이 끊어져 있으면 보통 0x00 또는 0xFF만 읽힌다 */
	if ((partnum == 0x00 && version == 0x00) ||
	    (partnum == 0xFF && version == 0xFF)) {
		dev_err(&cc->spi->dev,
			"리셋 후 응답 없음 (PARTNUM=0x%02x VERSION=0x%02x), 배선을 확인하세요\n",
			partnum, version);
		return -ENODEV;
	}

	dev_info(&cc->spi->dev, "CC1101 감지됨: PARTNUM=0x%02x VERSION=0x%02x\n",
		 partnum, version);
	return 0;
}

/*
 * 433.92MHz / 2-FSK / 38.4kbps 기준 참고 레지스터 값 (TI SmartRF Studio에서
 * 흔히 쓰이는 표준 설정과 동일한 계산식/상수를 사용). 실제 RF 환경에 맞춰
 * SmartRF Studio로 재계산 후 교체하는 것을 권장한다.
 *
 * IOCFG0 = 0x06 : 동기워드 송/수신 시 assert, 패킷 끝에서 deassert
 *                 -> TX 완료 및 (GDO2 미배선 시) RX 완료 알림에 공용으로 사용
 * IOCFG2 = 0x07 : CRC OK 패킷 수신 시 assert, FIFO 첫 바이트 읽으면 deassert
 *                 -> GDO2가 배선된 경우 RX 완료 전용 알림으로 사용
 */
static const u8 cc1101_default_regs[CC1101_NUM_CONFIG_REGS] = {
	[CC1101_IOCFG2]   = 0x07,
	[CC1101_IOCFG1]   = 0x2E,	/* 미사용: 3-state */
	[CC1101_IOCFG0]   = 0x06,
	[CC1101_FIFOTHR]  = 0x47,
	/* [변경 2026-08-16] 싱크워드를 TI 레퍼런스 기본값(0xD3/0x91)에서
	 * OTA 전용 값(0x2D/0xD4)으로 분리.
	 *
	 * 이유: 같은 프로젝트의 다른 팀 장비들도 433.92MHz + 같은 레퍼런스
	 * 설정을 쓰다 보니 싱크워드까지 전부 0xD3/0x91로 겹쳐 있었다. 그래서
	 * 우리 수신기가 남의 패킷("FHSS"=0x46485353로 시작하는 것,
	 * 0xA5로 시작하는 49byte짜리 등)을 CRC까지 통과시켜 받아버렸고,
	 * 커널 RX 큐가 남의 트래픽으로 가득 차 우리 OTA 패킷이 폐기됐다
	 * (실기기 확인: GDO2 인터럽트 1182회 중 대부분이 남의 패킷).
	 *
	 * CC1101은 싱크워드가 다르면 하드웨어 단에서 아예 무시한다 —
	 * 인터럽트도 안 울리고 FIFO에도 안 들어온다. 소프트웨어 필터링보다
	 * 훨씬 깨끗하고 CPU/큐 부담이 0이다.
	 *
	 * 0x2DD4는 1이 8개/0이 8개로 균형이 잡혀 있고 반복 패턴이 없어
	 * 오검출(false sync)이 적다.
	 *
	 * [중요] 이 값은 gateway-ota의 SpidevTransport(우회 구현)에 있는
	 * 레지스터 배열과 반드시 같아야 한다. 한쪽만 바꾸면 두 경로가 서로
	 * 통신하지 못한다.
	 */
	[CC1101_SYNC1]    = 0x2D,
	[CC1101_SYNC0]    = 0xD4,
	[CC1101_PKTLEN]   = CC1101_MAX_PACKET_LEN,	/* 하드웨어 FIFO(64B) 한도에 맞춘 상한 */
	/* [2026-08-16] ADR_CHK를 "주소일치"로 켜두면, ota_protocol.h 패킷은
	 * 페이로드 첫 바이트가 CC1101 주소필터용 주소 바이트가 아니라서 하드웨어가
	 * CRC 검사도 하기 전에 죄다 버림 (GDO2 "CRC OK 수신" 인터럽트가 한 번도
	 * 안 울리는 것으로 실기기 확인). SpidevTransport 임시 우회 코드에서도
	 * 이미 같은 이유로 꺼뒀던 것과 동일 — 주소필터 자체를 꺼서 모든 패킷을
	 * 받도록 함. */
	[CC1101_PKTCTRL1] = 0x0C,	/* APPEND_STATUS=1, CRC_AUTOFLUSH=1, ADR_CHK=끔(필터 없음) */
	[CC1101_PKTCTRL0] = 0x05,	/* 가변 길이 패킷, CRC enable */
	[CC1101_ADDR]     = 0x00,
	/* 채널 0 = 기준 주파수 그대로(433.92MHz).
	 *
	 * [검토했다가 안 쓴 것, 2026-08-16] 다른 팀과의 물리적 분리를 위해
	 * 채널 이동을 검토했으나, 싱크워드 분리(위 SYNC1/SYNC0)로 충분하다고
	 * 판단해 0을 유지한다.
	 *
	 * 나중에 채널을 옮길 일이 있으면 아래 제약을 반드시 확인할 것:
	 *   주파수 = 433.92MHz + CHANNR x 채널간격
	 *   채널간격 = (26MHz/2^18) x (256 + CHANSPC_M) x 2^CHANSPC_E ~= 200kHz
	 *              (CHANSPC_E = MDMCFG1[1:0] = 2, CHANSPC_M = MDMCFG0 = 0xF8)
	 *
	 * 국내 433MHz ISM 밴드가 433.05~434.79MHz이므로 **채널 4(434.72MHz)가
	 * 상한**이다. 채널 5는 밴드를 벗어나 전파법 위반이고, 큰 값(예: 200)은
	 * 473.92MHz로 CC1101 지원 밴드(387~464MHz)조차 벗어나 PLL이 락을 못 걸어
	 * 송수신이 통째로 죽는다. 채널 번호를 함부로 올리면 안 된다.
	 */
	[CC1101_CHANNR]   = 0x00,
	[CC1101_FSCTRL1]  = 0x06,
	[CC1101_FSCTRL0]  = 0x00,
	[CC1101_FREQ2]    = 0x10,	/* 433.92MHz (26MHz XOSC 기준) */
	[CC1101_FREQ1]    = 0xB0,
	[CC1101_FREQ0]    = 0x71,
	[CC1101_MDMCFG4]  = 0xCA,
	[CC1101_MDMCFG3]  = 0x83,
	[CC1101_MDMCFG2]  = 0x13,	/* 2-FSK, 16/16 sync word */
	[CC1101_MDMCFG1]  = 0x22,
	[CC1101_MDMCFG0]  = 0xF8,
	[CC1101_DEVIATN]  = 0x35,
	[CC1101_MCSM2]    = 0x07,
	[CC1101_MCSM1]    = 0x3F,	/* TX/RX 종료 후 자동으로 RX 유지, CCA 없음 */
	[CC1101_MCSM0]    = 0x18,	/* IDLE->{RX,TX} 전이 시 FS 자동 캘리브레이션 */
	[CC1101_FOCCFG]   = 0x16,
	[CC1101_BSCFG]    = 0x6C,
	[CC1101_AGCCTRL2] = 0x43,
	[CC1101_AGCCTRL1] = 0x40,
	[CC1101_AGCCTRL0] = 0x91,
	[CC1101_WOREVT1]  = 0x87,
	[CC1101_WOREVT0]  = 0x6B,
	[CC1101_WORCTRL]  = 0xFB,
	[CC1101_FREND1]   = 0x56,
	[CC1101_FREND0]   = 0x10,
	[CC1101_FSCAL3]   = 0xE9,
	[CC1101_FSCAL2]   = 0x2A,
	[CC1101_FSCAL1]   = 0x00,
	[CC1101_FSCAL0]   = 0x1F,
	[CC1101_RCCTRL1]  = 0x41,
	[CC1101_RCCTRL0]  = 0x00,
	[CC1101_FSTEST]   = 0x59,
	[CC1101_PTEST]    = 0x7F,
	[CC1101_AGCTEST]  = 0x3F,
	[CC1101_TEST2]    = 0x81,
	[CC1101_TEST1]    = 0x35,
	[CC1101_TEST0]    = 0x09,
};

int cc1101_load_default_config(struct cc1101 *cc)
{
	int ret;
	u8 pa_table[1] = { 0xC0 };	/* 대략 +10dBm, ERP 규정에 맞춰 조정 필요 */

	ret = cc1101_strobe(cc, CC1101_SIDLE);
	if (ret)
		return ret;

	ret = cc1101_write_burst(cc, CC1101_IOCFG2, cc1101_default_regs,
				  sizeof(cc1101_default_regs));
	if (ret)
		return ret;

	ret = cc1101_write_burst(cc, CC1101_PATABLE, pa_table, sizeof(pa_table));
	if (ret)
		return ret;

	return cc1101_strobe(cc, CC1101_SCAL);
}

int cc1101_enter_rx(struct cc1101 *cc)
{
	int ret = cc1101_strobe(cc, CC1101_SRX);

	if (!ret)
		cc->state = CC1101_STATE_RX;
	return ret;
}

int cc1101_enter_idle(struct cc1101 *cc)
{
	int ret = cc1101_strobe(cc, CC1101_SIDLE);

	if (!ret)
		cc->state = CC1101_STATE_IDLE;
	return ret;
}

/* freq_word = round(freq_hz * 2^16 / f_xosc), datasheet 식 (21) */
int cc1101_set_freq_hz(struct cc1101 *cc, u32 freq_hz)
{
	u64 word64 = ((u64)freq_hz << 16) + (CC1101_XOSC_HZ / 2);
	u32 freq_word = div_u64(word64, CC1101_XOSC_HZ);
	u8 regs[3] = {
		(freq_word >> 16) & 0xFF,
		(freq_word >> 8) & 0xFF,
		freq_word & 0xFF,
	};
	enum cc1101_state prev = cc->state;
	int ret;

	ret = cc1101_enter_idle(cc);
	if (ret)
		return ret;

	ret = cc1101_write_burst(cc, CC1101_FREQ2, regs, sizeof(regs));
	if (ret)
		return ret;

	ret = cc1101_strobe(cc, CC1101_SCAL);
	if (ret)
		return ret;

	if (prev == CC1101_STATE_RX)
		ret = cc1101_enter_rx(cc);

	return ret;
}

int cc1101_set_addr_filter(struct cc1101 *cc, u8 mode)
{
	u8 pktctrl1;
	int ret;

	if (mode > CC1101_ADRCHK_ADDR_BCAST0_FF)
		return -EINVAL;

	ret = cc1101_read_reg(cc, CC1101_PKTCTRL1, &pktctrl1);
	if (ret)
		return ret;

	pktctrl1 &= ~CC1101_PKTCTRL1_ADRCHK_MASK;
	pktctrl1 |= mode;

	return cc1101_write_reg(cc, CC1101_PKTCTRL1, pktctrl1);
}

int cc1101_read_rssi_dbm(struct cc1101 *cc, s8 *dbm)
{
	u8 raw;
	int ret = cc1101_read_status_reg(cc, CC1101_RSSI, &raw);

	if (ret)
		return ret;

	/* datasheet 17.3 RSSI formula */
	if (raw >= 128)
		*dbm = (s8)(((int)raw - 256) / 2 - 74);
	else
		*dbm = (s8)((int)raw / 2 - 74);

	return 0;
}
