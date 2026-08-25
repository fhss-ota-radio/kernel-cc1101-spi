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

#define CC1101_IOC_MAGIC	0xC1
#define CC1101_FHSS_VERSION	2
#define CC1101_FHSS_MAX_CHANNELS	255
#define CC1101_FHSS_ALGORITHM_SEEDED_PERMUTATION	1
#define CC1101_FHSS_ROLE_MASTER	1
#define CC1101_FHSS_ROLE_SLAVE	2
#define CC1101_FHSS_ALGORITHM_VERSION	1
#define CC1101_FHSS_SYNC_VERSION	1
#define CC1101_FHSS_DEFAULT_SEED	0x46485353U
#define CC1101_FHSS_ZERO_SEED_FALLBACK	0x6D2B79F5U
#define CC1101_FHSS_SYNC_PACKET_TYPE	10
#define CC1101_FHSS_SYNC_PACKET_SIZE	13

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

struct cc1101_fhss_rf_profile {
	__u32 base_freq_hz;	/* FHSS 모드에서 채널 0 기준 주파수 (Hz) */
	__u32 channel_spacing_hz;	/* FHSS 모드에서 채널 간 간격 (Hz) */

	__u16 sync_word;		/* FHSS 모드에서 싱크워드 (16bit) */
	__u8 mdmcfg4;
	__u8 mdmcfg3;
	__u8 pktctrl1;
	__u8 pktctrl0;

};

struct cc1101_fhss_hop_policy {
	__u32 seed;			/* ESP32와 공유할 호핑 순서 seed */
	__u32 slot_duration_us;		/* 한 채널을 사용하는 시간, 현재 300000us */
	__u32 channel_switch_guard_us;	/* 슬롯 경계보다 먼저 채널을 바꿀 시간 */
	__u16 channel_count;
	__u8 first_channel;		/* 순서의 첫 채널이자 랑데부 채널 */
	__u8 rendezvous_channel;	/* 동기 상실 시 돌아와 SYNC를 찾는 채널 */
	__u8 reserved_channel;		/* OTA 전용 등 호핑에서 제외할 채널 */
	__u8 algorithm_version;		/* 반드시 CC1101_FHSS_ALGORITHM_VERSION */
	__u8 channel_profile_id;	/* 팀에서 정한 주파수/채널 묶음 식별자 */
	__u8 reserved;
};

struct cc1101_fhss_config {
	__u16 version;		/* 구조체 버전 (현재 CC1101_FHSS_VERSION=2) */
	__u16 size;
	__u32 generation;
	__u32 algorithm_id;

	struct cc1101_fhss_rf_profile rf;
	struct cc1101_fhss_hop_policy hop;
};

struct cc1101_fhss_status {
	__u8 enabled;		/* FHSS 모드 활성화 여부 */
	__u8 synchronized;	/* 현재 채널이 싱크워드와 동기화되었는지 여부 */
	__u8 current_channel;	/* 현재 채널 번호 (0~255) */
	__u8 role;

	__u32 generation;		/* 현재 FHSS 세션의 generation 값 */
	__u64 current_slot;		/* 현재 FHSS 세션에서의 슬롯 번호 */
	__s32 last_error;		/* 마지막 FHSS 오류 코드 (0이면 정상) */
	__u32 sync_misses;		/* 연속으로 놓친 SYNC 개수 */
	__u32 sync_packets;		/* 정상 처리한 SYNC 개수 */
};

#define CC1101_IOC_FHSS_SET_CONFIG _IOW(CC1101_IOC_MAGIC, 14, struct cc1101_fhss_config)
#define CC1101_IOC_FHSS_START _IOW(CC1101_IOC_MAGIC, 15, __u8)
#define CC1101_IOC_FHSS_STOP _IO(CC1101_IOC_MAGIC, 16)
#define CC1101_IOC_FHSS_GET_STATUS _IOR(CC1101_IOC_MAGIC, 17, struct cc1101_fhss_status)



/* CC1101_IOC_SET_ADDR_FILTER 값 */
#define CC1101_ADDR_FILTER_DISABLE		0	/* 필터 끔: 1:N 브로드캐스트 수신 */
#define CC1101_ADDR_FILTER_ADDR_ONLY		1	/* ADDR 일치만 수신 */
#define CC1101_ADDR_FILTER_ADDR_BCAST0		2	/* ADDR 또는 0x00 브로드캐스트 수신 */
#define CC1101_ADDR_FILTER_ADDR_BCAST0_FF	3	/* ADDR 또는 0x00/0xFF 브로드캐스트 수신 */

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
