/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cc1101.h - TI CC1101 sub-1GHz 트랜시버 내부 정의 (레지스터 맵, 스트로브, 드라이버 상태)
 */
#ifndef _CC1101_H_
#define _CC1101_H_

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>

/* ---------------------------------------------------------------------
 * SPI 헤더 바이트 (datasheet 10.1 - Address Space)
 * bit7 : R/W  (1=read, 0=write)
 * bit6 : burst (1=burst, 0=single)
 * bit5:0 : 레지스터 주소
 * ------------------------------------------------------------------- */
#define CC1101_WRITE_SINGLE	0x00
#define CC1101_WRITE_BURST	0x40
#define CC1101_READ_SINGLE	0x80
#define CC1101_READ_BURST	0xC0

/* Configuration registers (0x00 - 0x2E) */
#define CC1101_IOCFG2		0x00
#define CC1101_IOCFG1		0x01
#define CC1101_IOCFG0		0x02
#define CC1101_FIFOTHR		0x03
#define CC1101_SYNC1		0x04
#define CC1101_SYNC0		0x05
#define CC1101_PKTLEN		0x06
#define CC1101_PKTCTRL1	0x07
#define CC1101_PKTCTRL0	0x08
#define CC1101_ADDR		0x09
#define CC1101_CHANNR		0x0A
#define CC1101_FSCTRL1		0x0B
#define CC1101_FSCTRL0		0x0C
#define CC1101_FREQ2		0x0D
#define CC1101_FREQ1		0x0E
#define CC1101_FREQ0		0x0F
#define CC1101_MDMCFG4		0x10
#define CC1101_MDMCFG3		0x11
#define CC1101_MDMCFG2		0x12
#define CC1101_MDMCFG1		0x13
#define CC1101_MDMCFG0		0x14
#define CC1101_DEVIATN		0x15
#define CC1101_MCSM2		0x16
#define CC1101_MCSM1		0x17
#define CC1101_MCSM0		0x18
#define CC1101_FOCCFG		0x19
#define CC1101_BSCFG		0x1A
#define CC1101_AGCCTRL2	0x1B
#define CC1101_AGCCTRL1	0x1C
#define CC1101_AGCCTRL0	0x1D
#define CC1101_WOREVT1		0x1E
#define CC1101_WOREVT0		0x1F
#define CC1101_WORCTRL		0x20
#define CC1101_FREND1		0x21
#define CC1101_FREND0		0x22
#define CC1101_FSCAL3		0x23
#define CC1101_FSCAL2		0x24
#define CC1101_FSCAL1		0x25
#define CC1101_FSCAL0		0x26
#define CC1101_RCCTRL1		0x27
#define CC1101_RCCTRL0		0x28
#define CC1101_FSTEST		0x29
#define CC1101_PTEST		0x2A
#define CC1101_AGCTEST		0x2B
#define CC1101_TEST2		0x2C
#define CC1101_TEST1		0x2D
#define CC1101_TEST0		0x2E

#define CC1101_NUM_CONFIG_REGS	(CC1101_TEST0 + 1)	/* 0x00..0x2E -> 47 */

/* Command strobes (0x30 - 0x3D) */
#define CC1101_SRES		0x30	/* 리셋 */
#define CC1101_SFSTXON	0x31
#define CC1101_SXOFF		0x32
#define CC1101_SCAL		0x33	/* 주파수 합성기 캘리브레이션 */
#define CC1101_SRX		0x34	/* RX 모드 진입 */
#define CC1101_STX		0x35	/* TX 모드 진입 */
#define CC1101_SIDLE		0x36	/* IDLE 진입 */
#define CC1101_SWOR		0x38
#define CC1101_SPWD		0x39	/* 파워 다운 */
#define CC1101_SFRX		0x3A	/* RX FIFO flush (IDLE 상태에서만) */
#define CC1101_SFTX		0x3B	/* TX FIFO flush (IDLE 상태에서만) */
#define CC1101_SWORRST		0x3C
#define CC1101_SNOP		0x3D

/* Status registers - 읽을 때 반드시 burst 비트를 세워야 strobe와 구분됨 */
#define CC1101_PARTNUM		0x30
#define CC1101_VERSION		0x31
#define CC1101_FREQEST		0x32
#define CC1101_LQI		0x33
#define CC1101_RSSI		0x34
#define CC1101_MARCSTATE	0x35
#define CC1101_PKTSTATUS	0x38
#define CC1101_TXBYTES		0x3A
#define CC1101_RXBYTES		0x3B

#define CC1101_PATABLE		0x3E
#define CC1101_TXFIFO		0x3F
#define CC1101_RXFIFO		0x3F

#define CC1101_RXBYTES_OVERFLOW	BIT(7)
#define CC1101_RXBYTES_MASK	0x7F
#define CC1101_MARCSTATE_MASK	0x1F
#define CC1101_MARCSTATE_RXFIFO_OVERFLOW	0x11
#define CC1101_LQI_CRC_OK	BIT(7)
#define CC1101_LQI_MASK	0x7F

/* PKTCTRL1 주소 필터 모드 (bit1:0, ADR_CHK) */
#define CC1101_ADRCHK_NONE		0x00	/* 필터 비활성화 - 1:N 브로드캐스트 */
#define CC1101_ADRCHK_ADDR		0x01
#define CC1101_ADRCHK_ADDR_BCAST0	0x02
#define CC1101_ADRCHK_ADDR_BCAST0_FF	0x03
#define CC1101_PKTCTRL1_ADRCHK_MASK	0x03

#define CC1101_XOSC_HZ		26000000UL	/* 보드 기본 크리스탈 (26MHz) */
#define CC1101_MAX_PACKET_LEN	61		/* 64바이트 FIFO - 길이바이트 - 상태 2바이트 */
#define CC1101_TX_TIMEOUT_MS	500

enum cc1101_state {
	CC1101_STATE_IDLE,
	CC1101_STATE_RX,
	CC1101_STATE_TX,
};

/* read()가 반환하는 한 개의 수신 패킷을 rx_fifo에 넣을 때 쓰는 프레이밍: [len][payload...] */
#define CC1101_RX_FIFO_SIZE	4096

struct cc1101;
struct cc1101_fhss;
int cc1101_switch_channel(struct cc1101 *cc, u8 channel);
int cc1101_transmit_packet(struct cc1101 *cc, const u8 *payload, size_t len);

struct cc1101 {
	struct spi_device	*spi;
	struct miscdevice	miscdev;
	char			miscdev_name[16];

	struct mutex		lock;		/* SPI 버스 + 칩 상태 보호 */
	/* 사용자 write()와 FHSS SYNC가 동시에 송신을 시작하지 않도록 TX 한 건의
	 * 시작부터 GDO0 완료까지 직렬화한다. lock은 IRQ가 상태를 갱신할 때도
	 * 쓰므로 송신 완료를 기다리는 동안 유지할 수 없어 별도 mutex가 필요하다. */
	struct mutex		tx_lock;
	enum cc1101_state	state;

	struct gpio_desc	*gdo0;		/* 필수: 패킷 수신/송신완료 알림 */
	struct gpio_desc	*gdo2;		/* 선택: RX CRC OK 전용 알림 */
	int			irq_gdo0;
	int			irq_gdo2;

	struct completion	tx_done;
	int			tx_result;
	wait_queue_head_t	rx_wait;
	struct kfifo		rx_fifo;

	atomic_t		open_count;
	struct cc1101_fhss	*fhss;
};

/* cc1101_core.c 에서 제공 */
int cc1101_read_reg(struct cc1101 *cc, u8 addr, u8 *val);
int cc1101_write_reg(struct cc1101 *cc, u8 addr, u8 val);
int cc1101_read_status_reg(struct cc1101 *cc, u8 addr, u8 *val);
int cc1101_read_burst(struct cc1101 *cc, u8 addr, u8 *buf, size_t len);
int cc1101_write_burst(struct cc1101 *cc, u8 addr, const u8 *buf, size_t len);
int cc1101_strobe(struct cc1101 *cc, u8 strobe);

int cc1101_hw_reset(struct cc1101 *cc);
int cc1101_load_default_config(struct cc1101 *cc);
int cc1101_enter_rx(struct cc1101 *cc);
int cc1101_enter_rx_recover(struct cc1101 *cc);
int cc1101_enter_idle(struct cc1101 *cc);
int cc1101_set_freq_hz(struct cc1101 *cc, u32 freq_hz);
int cc1101_set_channel_spacing_hz(struct cc1101 *cc, u32 spacing_hz);
int cc1101_set_addr_filter(struct cc1101 *cc, u8 mode);
int cc1101_read_rssi_dbm(struct cc1101 *cc, s8 *dbm);

#endif /* _CC1101_H_ */
