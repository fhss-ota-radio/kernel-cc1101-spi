#ifndef _CC1101_FHSS_H_
#define _CC1101_FHSS_H_

#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/mutex.h>

#include "cc1101_ioctl.h"

/* 세 번 확인 후 동기화하고, 다섯 슬롯 연속 누락 시 랑데부 채널로 돌아간다. */
#define CC1101_FHSS_SYNC_ACQUIRE_COUNT	3
#define CC1101_FHSS_SYNC_LOSS_COUNT	5
#define CC1101_FHSS_MAX_CORRECTION_US	500

struct cc1101;
struct cc1101_hop_algorithm;

enum cc1101_fhss_state {
	CC1101_FHSS_DISABLED,
	CC1101_FHSS_CONFIGURED,
	CC1101_FHSS_SEARCHING,
	/* 첫 SYNC는 받았지만 아직 3회 확인이 끝나지 않은 상태다.
	 * 이때부터는 MASTER의 슬롯/채널을 따라가야 다음 SYNC도 받을 수 있다. */
	CC1101_FHSS_ACQUIRING,
	CC1101_FHSS_SYNCHRONIZED,
	CC1101_FHSS_STOPPING,
};

struct cc1101_fhss {
	struct cc1101 *cc;

	struct mutex config_lock;
	struct cc1101_fhss_config config;
	const struct cc1101_hop_algorithm *algorithm;

	enum cc1101_fhss_state state;
	u64 reference_time_ns;
	u64 current_slot;
	u8 current_channel;
	u8 role;
	s32 last_error;
	u16 sync_sequence;
	u16 last_rx_sequence;
	bool have_last_rx_sequence;
	u32 sync_packets;
	u32 sync_misses;
	u32 acquire_progress;
	u64 last_sync_slot;

	u8 permutation[CC1101_FHSS_MAX_CHANNELS];

	struct hrtimer timer;
	struct kthread_worker worker;
	struct kthread_work hop_work;
	struct task_struct *worker_task;
};

int cc1101_fhss_init(struct cc1101 *cc);
void cc1101_fhss_destroy(struct cc1101 *cc);

int cc1101_fhss_set_config(
	struct cc1101 *cc,
	const struct cc1101_fhss_config *config);

int cc1101_fhss_start(struct cc1101 *cc, u8 role);
int cc1101_fhss_stop(struct cc1101 *cc);

void cc1101_fhss_get_status(
	struct cc1101 *cc,
	struct cc1101_fhss_status *status);

/* RX IRQ가 받은 13바이트 FHSS SYNC를 드라이버 내부에서 소비한다.
 * 일반 사용자 데이터는 false를 반환해서 기존 read() 큐로 그대로 보낸다. */
bool cc1101_fhss_is_sync_packet(struct cc1101 *cc,
				const u8 *payload, size_t len);
void cc1101_fhss_handle_sync(struct cc1101 *cc, const u8 *payload,
			     size_t len, u64 rx_time_ns);

#endif
