#ifndef _CC1101_FHSS_H_
#define _CC1101_FHSS_H_

#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/mutex.h>

#include "cc1101_ioctl.h"

struct cc1101;
struct cc1101_hop_algorithm;

enum cc1101_fhss_state {
	CC1101_FHSS_DISABLED,
	CC1101_FHSS_CONFIGURED,
	CC1101_FHSS_SEARCHING,
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

#endif
