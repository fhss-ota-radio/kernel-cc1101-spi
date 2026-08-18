/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cc1101_ioctl.h - /dev/cc1101 유저 스페이스 ioctl 인터페이스
 *
 * 커널 모듈과 유저 애플리케이션이 함께 include 하는 UAPI 헤더.
 */
#ifndef _CC1101_IOCTL_H_
#define _CC1101_IOCTL_H_

#include <linux/types.h>
#include <linux/ioctl.h>

struct cc1101_reg_io {
	__u8 addr;	/* 0x00-0x2E config reg, 0x3E PATABLE */
	__u8 value;
};

struct cc1101_freq_cfg {
	__u32 freq_hz;	/* 목표 캐리어 주파수 (Hz), 예: 433920000 */
};

struct cc1101_status {
	__s8 rssi_dbm;
	__u8 lqi;		/* 0-127, 값이 낮을수록 링크 품질 좋음 */
	__u8 crc_ok;		/* 마지막 수신 패킷 CRC 결과 */
	__u8 marc_state;	/* 칩 내부 상태 머신 값 (MARCSTATE) */
};

/* CC1101_IOC_SET_ADDR_FILTER 값 */
#define CC1101_ADDR_FILTER_DISABLE		0	/* 필터 끔: 1:N 브로드캐스트 수신 */
#define CC1101_ADDR_FILTER_ADDR_ONLY		1	/* ADDR 일치만 수신 */
#define CC1101_ADDR_FILTER_ADDR_BCAST0		2	/* ADDR 또는 0x00 브로드캐스트 수신 */
#define CC1101_ADDR_FILTER_ADDR_BCAST0_FF	3	/* ADDR 또는 0x00/0xFF 브로드캐스트 수신 */

#define CC1101_IOC_MAGIC	0xC1

#define CC1101_IOC_RESET		_IO(CC1101_IOC_MAGIC, 0)
#define CC1101_IOC_STROBE		_IOW(CC1101_IOC_MAGIC,  1, __u8)
#define CC1101_IOC_READ_REG		_IOWR(CC1101_IOC_MAGIC, 2, struct cc1101_reg_io)
#define CC1101_IOC_WRITE_REG		_IOW(CC1101_IOC_MAGIC,  3, struct cc1101_reg_io)
#define CC1101_IOC_SET_FREQ		_IOW(CC1101_IOC_MAGIC,  4, struct cc1101_freq_cfg)
#define CC1101_IOC_SET_CHANNEL		_IOW(CC1101_IOC_MAGIC,  5, __u8)
#define CC1101_IOC_SET_ADDR		_IOW(CC1101_IOC_MAGIC,  6, __u8)
#define CC1101_IOC_SET_ADDR_FILTER	_IOW(CC1101_IOC_MAGIC,  7, __u8)
#define CC1101_IOC_SET_PA_POWER	_IOW(CC1101_IOC_MAGIC,  8, __u8)
#define CC1101_IOC_GET_STATUS		_IOR(CC1101_IOC_MAGIC,  9, struct cc1101_status)
#define CC1101_IOC_SET_RX		_IO(CC1101_IOC_MAGIC, 10)
#define CC1101_IOC_SET_IDLE		_IO(CC1101_IOC_MAGIC, 11)
#define CC1101_IOC_FLUSH_RX		_IO(CC1101_IOC_MAGIC, 12)
#define CC1101_IOC_FLUSH_TX		_IO(CC1101_IOC_MAGIC, 13)

#endif /* _CC1101_IOCTL_H_ */
