// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "cc1101.h"
#include "cc1101_fhss.h"
#include "cc1101_hop.h"

static void cc1101_fhss_schedule_next(struct cc1101_fhss *fhss)
{
	u64 slot_ns = (u64)fhss->config.hop.slot_duration_us * NSEC_PER_USEC;
	u64 next_ns = fhss->reference_time_ns +
		(fhss->current_slot + 1) * slot_ns;

	hrtimer_start(&fhss->timer, ns_to_ktime(next_ns), HRTIMER_MODE_ABS);
}

static enum hrtimer_restart cc1101_fhss_timer_callback(
	struct hrtimer *timer)
{
	struct cc1101_fhss *fhss =
		container_of(timer, struct cc1101_fhss, timer);

	kthread_queue_work(&fhss->worker, &fhss->hop_work);
	return HRTIMER_NORESTART;
}

static void cc1101_fhss_hop_worker(struct kthread_work *work)
{
	struct cc1101_fhss *fhss =
		container_of(work, struct cc1101_fhss, hop_work);
	u64 slot_ns, now_ns, slot;
	u8 channel;
	int ret;

	mutex_lock(&fhss->config_lock);
	if (fhss->state != CC1101_FHSS_SYNCHRONIZED) {
		mutex_unlock(&fhss->config_lock);
		return;
	}

	slot_ns = (u64)fhss->config.hop.slot_duration_us * NSEC_PER_USEC;
	now_ns = ktime_get_ns();
	slot = now_ns > fhss->reference_time_ns ?
		div64_u64(now_ns - fhss->reference_time_ns, slot_ns) : 0;
	fhss->current_slot = slot;

	ret = fhss->algorithm->channel_for_slot(fhss, slot, &channel);
	if (ret)
		goto failed;

	mutex_lock(&fhss->cc->lock);
	if (fhss->cc->state == CC1101_STATE_TX)
		ret = -EBUSY;
	else
		ret = cc1101_switch_channel(fhss->cc, channel);
	mutex_unlock(&fhss->cc->lock);
	if (ret == -EBUSY) {
		/* 송신을 중단하지 않고 다음 절대 슬롯에서 다시 동기화한다. */
		cc1101_fhss_schedule_next(fhss);
		mutex_unlock(&fhss->config_lock);
		return;
	}
	if (ret)
		goto failed;

	fhss->current_channel = channel;
	cc1101_fhss_schedule_next(fhss);
	mutex_unlock(&fhss->config_lock);
	return;

failed:
	fhss->last_error = ret;
	fhss->state = CC1101_FHSS_CONFIGURED;
	mutex_unlock(&fhss->config_lock);
}

static int cc1101_fhss_validate_config(
	const struct cc1101_fhss_config *config)
{
	u32 last_channel;

	if (config->version != CC1101_FHSS_VERSION ||
	    config->size != sizeof(*config))
		return -EINVAL;
	if (!config->hop.slot_duration_us || !config->hop.channel_count ||
	    config->hop.channel_count > CC1101_FHSS_MAX_CHANNELS)
		return -EINVAL;

	last_channel = config->hop.first_channel +
		config->hop.channel_count - 1;
	if (last_channel > 255)
		return -ERANGE;
	if (!config->rf.base_freq_hz || !config->rf.channel_spacing_hz)
		return -EINVAL;
	if (!cc1101_hop_get_algorithm(config->algorithm_id))
		return -EOPNOTSUPP;

	return 0;
}

static int cc1101_fhss_apply_profile(struct cc1101_fhss *fhss)
{
	const struct cc1101_fhss_rf_profile *rf = &fhss->config.rf;
	struct cc1101 *cc = fhss->cc;
	int ret, rx_ret;

	mutex_lock(&cc->lock);
	ret = cc1101_enter_idle(cc);
	if (!ret)
		ret = cc1101_set_freq_hz(cc, rf->base_freq_hz);
	if (!ret)
		ret = cc1101_write_reg(cc, CC1101_SYNC1, rf->sync_word >> 8);
	if (!ret)
		ret = cc1101_write_reg(cc, CC1101_SYNC0, rf->sync_word & 0xff);
	if (!ret)
		ret = cc1101_write_reg(cc, CC1101_MDMCFG4, rf->mdmcfg4);
	if (!ret)
		ret = cc1101_write_reg(cc, CC1101_MDMCFG3, rf->mdmcfg3);
	if (!ret)
		ret = cc1101_set_channel_spacing_hz(cc,
						   rf->channel_spacing_hz);
	if (!ret)
		ret = cc1101_write_reg(cc, CC1101_PKTCTRL1, rf->pktctrl1);
	if (!ret)
		ret = cc1101_write_reg(cc, CC1101_PKTCTRL0, rf->pktctrl0);
	rx_ret = cc1101_enter_rx(cc);
	if (!ret)
		ret = rx_ret;
	mutex_unlock(&cc->lock);

	return ret;
}

int cc1101_fhss_init(struct cc1101 *cc)
{
	struct cc1101_fhss *fhss;

	fhss = kzalloc(sizeof(*fhss), GFP_KERNEL);
	if (!fhss)
		return -ENOMEM;

	fhss->cc = cc;
	fhss->state = CC1101_FHSS_DISABLED;
	mutex_init(&fhss->config_lock);
	kthread_init_worker(&fhss->worker);
	kthread_init_work(&fhss->hop_work, cc1101_fhss_hop_worker);
	hrtimer_init(&fhss->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	fhss->timer.function = cc1101_fhss_timer_callback;

	fhss->worker_task = kthread_run(kthread_worker_fn, &fhss->worker,
					"cc1101-fhss");
	if (IS_ERR(fhss->worker_task)) {
		int ret = PTR_ERR(fhss->worker_task);

		kfree(fhss);
		return ret;
	}

	cc->fhss = fhss;
	return 0;
}

void cc1101_fhss_destroy(struct cc1101 *cc)
{
	struct cc1101_fhss *fhss = cc->fhss;

	if (!fhss)
		return;

	cc1101_fhss_stop(cc);
	kthread_flush_worker(&fhss->worker);
	kthread_stop(fhss->worker_task);
	cc->fhss = NULL;
	kfree(fhss);
}

int cc1101_fhss_set_config(struct cc1101 *cc,
			   const struct cc1101_fhss_config *config)
{
	struct cc1101_fhss *fhss = cc->fhss;
	const struct cc1101_hop_algorithm *algorithm;
	int ret;

	if (!fhss)
		return -ENODEV;
	ret = cc1101_fhss_validate_config(config);
	if (ret)
		return ret;
	algorithm = cc1101_hop_get_algorithm(config->algorithm_id);

	mutex_lock(&fhss->config_lock);
	if (fhss->state == CC1101_FHSS_SYNCHRONIZED ||
	    fhss->state == CC1101_FHSS_STOPPING) {
		ret = -EBUSY;
		goto out;
	}

	fhss->config = *config;
	fhss->algorithm = algorithm;
	fhss->last_error = 0;
	ret = fhss->algorithm->init(fhss);
	if (!ret)
		fhss->state = CC1101_FHSS_CONFIGURED;
out:
	mutex_unlock(&fhss->config_lock);
	return ret;
}

int cc1101_fhss_start(struct cc1101 *cc, u8 role)
{
	struct cc1101_fhss *fhss = cc->fhss;
	int ret;

	if (!fhss)
		return -ENODEV;
	if (role != CC1101_FHSS_ROLE_MASTER &&
	    role != CC1101_FHSS_ROLE_SLAVE)
		return -EINVAL;

	mutex_lock(&fhss->config_lock);
	if (fhss->state != CC1101_FHSS_CONFIGURED) {
		ret = -EINVAL;
		goto out;
	}

	ret = cc1101_fhss_apply_profile(fhss);
	if (ret)
		goto out;
	ret = fhss->algorithm->init(fhss);
	if (ret)
		goto out;

	fhss->role = role;
	fhss->last_error = 0;
	fhss->reference_time_ns = ktime_get_ns();
	fhss->state = CC1101_FHSS_SYNCHRONIZED;
	kthread_queue_work(&fhss->worker, &fhss->hop_work);
out:
	mutex_unlock(&fhss->config_lock);
	return ret;
}

int cc1101_fhss_stop(struct cc1101 *cc)
{
	struct cc1101_fhss *fhss = cc->fhss;

	if (!fhss)
		return -ENODEV;

	mutex_lock(&fhss->config_lock);
	if (fhss->state == CC1101_FHSS_DISABLED) {
		mutex_unlock(&fhss->config_lock);
		return 0;
	}
	fhss->state = CC1101_FHSS_STOPPING;
	mutex_unlock(&fhss->config_lock);

	hrtimer_cancel(&fhss->timer);
	kthread_cancel_work_sync(&fhss->hop_work);

	mutex_lock(&fhss->config_lock);
	fhss->state = fhss->algorithm ? CC1101_FHSS_CONFIGURED :
		CC1101_FHSS_DISABLED;
	mutex_unlock(&fhss->config_lock);
	return 0;
}

void cc1101_fhss_get_status(struct cc1101 *cc,
			    struct cc1101_fhss_status *status)
{
	struct cc1101_fhss *fhss = cc->fhss;

	memset(status, 0, sizeof(*status));
	if (!fhss) {
		status->last_error = -ENODEV;
		return;
	}

	mutex_lock(&fhss->config_lock);
	status->enabled = fhss->state == CC1101_FHSS_SYNCHRONIZED;
	status->synchronized = status->enabled;
	status->current_channel = fhss->current_channel;
	status->role = fhss->role;
	status->generation = fhss->config.generation;
	status->current_slot = fhss->current_slot;
	status->last_error = fhss->last_error;
	mutex_unlock(&fhss->config_lock);
}
