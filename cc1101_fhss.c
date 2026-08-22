// SPDX-License-Identifier: GPL-2.0
/*
 * FHSS(주파수 호핑) 동작 순서
 *
 * 1. 사용자 프로그램이 SET_CONFIG ioctl로 채널 수, 순서 생성용 seed,
 *    채널 유지 시간(slot_duration)을 전달한다.
 * 2. MASTER는 랑데부 채널에서 SYNC를 보내고, SLAVE는 세 번 확인한다.
 * 3. SLAVE가 MASTER의 slot_number와 수신 시각으로 기준 시간을 맞춘다.
 * 4. 작업 스레드는 현재 슬롯에 맞는 채널을 계산해서 CC1101 채널을 바꾼다.
 * 5. STOP ioctl을 호출하면 타이머와 대기 중인 작업을 모두 멈춘다.
 *
 * 타이머 함수 안에서는 잠들 수 있는 SPI 통신을 할 수 없기 때문에, 타이머는
 * 작업 스레드를 깨우기만 하고 실제 채널 변경은 작업 스레드가 담당한다.
 */
#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "cc1101.h"
#include "cc1101_fhss.h"
#include "cc1101_hop.h"

/* ESP32와 공유하는 13바이트 SYNC wire format(Little Endian).
 * [type][version][generation:4][sequence:2][hop_index][slot:4] */
struct cc1101_fhss_sync {
	u32 generation;
	u16 sequence;
	u8 hop_index;
	u32 slot_number;
};

static void cc1101_put_le16(u8 *buf, u16 value)
{
	buf[0] = value & 0xff;
	buf[1] = value >> 8;
}

static void cc1101_put_le32(u8 *buf, u32 value)
{
	buf[0] = value & 0xff;
	buf[1] = (value >> 8) & 0xff;
	buf[2] = (value >> 16) & 0xff;
	buf[3] = value >> 24;
}

static u16 cc1101_get_le16(const u8 *buf)
{
	return (u16)buf[0] | ((u16)buf[1] << 8);
}

static u32 cc1101_get_le32(const u8 *buf)
{
	return (u32)buf[0] | ((u32)buf[1] << 8) |
	       ((u32)buf[2] << 16) | ((u32)buf[3] << 24);
}

static void cc1101_fhss_encode_sync(struct cc1101_fhss *fhss, u64 slot,
				    u8 *packet)
{
	u64 slot_tmp = slot;
	u32 hop_index = do_div(slot_tmp, fhss->config.hop.channel_count);

	packet[0] = CC1101_FHSS_SYNC_PACKET_TYPE;
	packet[1] = CC1101_FHSS_SYNC_VERSION;
	cc1101_put_le32(&packet[2], fhss->config.generation);
	cc1101_put_le16(&packet[6], fhss->sync_sequence++);
	packet[8] = hop_index;
	cc1101_put_le32(&packet[9], (u32)slot);
}

static int cc1101_fhss_decode_sync(const u8 *packet, size_t len,
				   struct cc1101_fhss_sync *sync)
{
	if (!packet || !sync || len != CC1101_FHSS_SYNC_PACKET_SIZE ||
	    packet[0] != CC1101_FHSS_SYNC_PACKET_TYPE ||
	    packet[1] != CC1101_FHSS_SYNC_VERSION)
		return -EINVAL;

	sync->generation = cc1101_get_le32(&packet[2]);
	sync->sequence = cc1101_get_le16(&packet[6]);
	sync->hop_index = packet[8];
	sync->slot_number = cc1101_get_le32(&packet[9]);
	return 0;
}

static void cc1101_fhss_schedule_next(struct cc1101_fhss *fhss)
{
	u64 slot_ns = (u64)fhss->config.hop.slot_duration_us * NSEC_PER_USEC;
	u64 guard_ns = (u64)fhss->config.hop.channel_switch_guard_us *
		NSEC_PER_USEC;
	u64 next_ns = fhss->reference_time_ns +
		(fhss->current_slot + 1) * slot_ns - guard_ns;

	/* "지금부터 한 슬롯 뒤"가 아니라 최초 시작 시각을 기준으로 예약한다.
	 * 그래야 작업이 조금 늦어져도 그 지연이 슬롯마다 계속 쌓이지 않는다. */
	hrtimer_start(&fhss->timer, ns_to_ktime(next_ns), HRTIMER_MODE_ABS);
}

static enum hrtimer_restart cc1101_fhss_timer_callback(
	struct hrtimer *timer)
{
	struct cc1101_fhss *fhss =
		container_of(timer, struct cc1101_fhss, timer);

	/* 실제 SPI 작업은 잠들 수 있으므로 작업 스레드에 넘긴다. */
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

	/* 타이머가 늦게 실행됐을 수도 있으므로 단순히 슬롯을 1 증가시키지 않고,
	 * 실제 경과 시간으로 지금 있어야 할 슬롯을 다시 계산한다. */
	slot_ns = (u64)fhss->config.hop.slot_duration_us * NSEC_PER_USEC;
	now_ns = ktime_get_ns();
	/* guard 시간만큼 일찍 깨워 다음 채널을 준비한다. 계산에도 guard를 더해야
	 * 타이머가 경계 직전에 실행됐을 때 이전 슬롯으로 되돌아가지 않는다. */
	slot = now_ns +
		(u64)fhss->config.hop.channel_switch_guard_us * NSEC_PER_USEC >
		fhss->reference_time_ns ?
		div64_u64(now_ns +
			(u64)fhss->config.hop.channel_switch_guard_us * NSEC_PER_USEC -
			fhss->reference_time_ns, slot_ns) : 0;
	fhss->current_slot = slot;

	ret = fhss->algorithm->channel_for_slot(fhss, slot, &channel);
	if (ret)
		goto failed;

	mutex_lock(&fhss->cc->lock);
	/* 송신 중 채널을 바꾸면 전송 중인 패킷이 끊긴다. 이 슬롯의 변경은
	 * 건너뛰고 다음 슬롯에서 현재 시각 기준 채널로 다시 맞춘다. */
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

	if (fhss->role == CC1101_FHSS_ROLE_MASTER) {
		u8 sync_packet[CC1101_FHSS_SYNC_PACKET_SIZE];

		/* MASTER가 매 슬롯의 채널과 번호를 알려준다. 사용자 앱은 이
		 * 제어 패킷을 직접 만들 필요 없이 평소처럼 write()만 사용한다.
		 * 채널은 guard만큼 먼저 바꿨으므로 명목 슬롯 경계까지 기다렸다가
		 * SYNC를 보내 SLAVE가 다음 채널을 준비할 시간을 일정하게 만든다. */
		if (fhss->config.hop.channel_switch_guard_us)
			usleep_range(fhss->config.hop.channel_switch_guard_us,
				fhss->config.hop.channel_switch_guard_us + 500);
		cc1101_fhss_encode_sync(fhss, slot, sync_packet);
		ret = cc1101_transmit_packet(fhss->cc, sync_packet,
					      sizeof(sync_packet));
		if (ret && ret != -EBUSY)
			goto failed;
	} else if (slot > fhss->last_sync_slot) {
		fhss->sync_misses++;
		if (slot > fhss->last_sync_slot +
		    CC1101_FHSS_SYNC_LOSS_COUNT) {
			/* SYNC를 다섯 슬롯 동안 못 받으면 계속 엉뚱한 채널을 돌지
			 * 않고 랑데부 채널로 돌아가 처음부터 다시 찾는다. */
			mutex_lock(&fhss->cc->lock);
			ret = cc1101_switch_channel(fhss->cc,
				fhss->config.hop.rendezvous_channel);
			mutex_unlock(&fhss->cc->lock);
			if (ret)
				goto failed;
			fhss->state = CC1101_FHSS_SEARCHING;
			fhss->current_channel =
				fhss->config.hop.rendezvous_channel;
			fhss->acquire_progress = 0;
			dev_warn(&fhss->cc->spi->dev,
				 "FHSS SYNC 상실, 채널 %u에서 재탐색\n",
				 fhss->current_channel);
			mutex_unlock(&fhss->config_lock);
			return;
		}
	}
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

	/* 잘못된 값으로 채널 계산을 시작하면 배열 범위를 벗어나거나 타이머가
	 * 계속 즉시 실행될 수 있으므로 설정을 저장하기 전에 검사한다. */
	if (config->version != CC1101_FHSS_VERSION ||
	    config->size != sizeof(*config))
		return -EINVAL;
	if (!config->hop.slot_duration_us || !config->hop.channel_count ||
	    config->hop.channel_count > CC1101_FHSS_MAX_CHANNELS)
		return -EINVAL;
	if (config->hop.algorithm_version != CC1101_FHSS_ALGORITHM_VERSION ||
	    config->hop.channel_switch_guard_us >=
		config->hop.slot_duration_us)
		return -EINVAL;

	last_channel = config->hop.first_channel +
		config->hop.channel_count - 1;
	if (last_channel > 255)
		return -ERANGE;
	/* 첫 채널은 셔플하지 않으므로 랑데부 채널도 반드시 첫 채널이다. */
	if (config->hop.rendezvous_channel != config->hop.first_channel)
		return -EINVAL;
	if (config->hop.reserved_channel >= config->hop.first_channel &&
	    config->hop.reserved_channel <= last_channel)
		return -EINVAL;
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

	/* 주파수 관련 레지스터를 바꾸는 동안 RX/TX가 시작되지 않도록 라디오를
	 * IDLE로 만든 뒤 한 번에 설정하고, 마지막에 다시 RX로 들어간다. */
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
	rx_ret = cc1101_enter_rx_recover(cc);
	if (!ret)
		ret = rx_ret;
	mutex_unlock(&cc->lock);

	return ret;
}

int cc1101_fhss_init(struct cc1101 *cc)
{
	struct cc1101_fhss *fhss;

	/* probe 때 장치마다 FHSS 상태와 전용 작업 스레드를 하나씩 만든다. */
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

	/* 메모리를 해제하기 전에 타이머와 작업을 먼저 완전히 멈춘다. */
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
	if (fhss->state == CC1101_FHSS_SEARCHING ||
	    fhss->state == CC1101_FHSS_SYNCHRONIZED ||
	    fhss->state == CC1101_FHSS_STOPPING) {
		ret = -EBUSY;
		goto out;
	}

	/* 설정과 seed로 만든 채널 순서를 함께 저장한다. 실행 중인 설정은
	 * 중간에 바꾸지 못하게 해서 송수신 채널 순서가 갑자기 달라지는 걸 막는다. */
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
	u8 sync_packet[CC1101_FHSS_SYNC_PACKET_SIZE];
	int i;
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

	/* 어느 시각에 START ioctl이 호출됐든 처음에는 모두 설정에 지정한
	 * rendezvous_channel로 모인다. 여기서 SYNC를 잡은 뒤에만 호핑한다. */
	mutex_lock(&cc->lock);
	ret = cc1101_switch_channel(cc,
				     fhss->config.hop.rendezvous_channel);
	mutex_unlock(&cc->lock);
	if (ret)
		goto out;

	fhss->role = role;
	fhss->last_error = 0;
	fhss->current_slot = 0;
	fhss->current_channel = fhss->config.hop.rendezvous_channel;
	fhss->sync_sequence = 0;
	fhss->last_rx_sequence = 0;
	fhss->have_last_rx_sequence = false;
	fhss->sync_packets = 0;
	fhss->sync_misses = 0;
	fhss->acquire_progress = 0;
	fhss->last_sync_slot = 0;

	if (role == CC1101_FHSS_ROLE_SLAVE) {
		/* SLAVE는 자체 START 시각을 믿지 않는다. 랑데부 채널에서 MASTER의
		 * SYNC 세 개를 받은 시각과 slot_number로 기준 시간을 만든다. */
		fhss->state = CC1101_FHSS_SEARCHING;
		dev_info(&cc->spi->dev,
			 "FHSS SLAVE: 채널 %u에서 SYNC 탐색 시작\n",
			 fhss->current_channel);
		goto out;
	}

	/* SLAVE가 시작 명령을 조금 늦게 처리해도 잡을 수 있도록 MASTER는
	 * 랑데부 채널에서 slot 0 SYNC를 세 번 먼저 보낸다. */
	for (i = 0; i < CC1101_FHSS_SYNC_ACQUIRE_COUNT; i++) {
		cc1101_fhss_encode_sync(fhss, 0, sync_packet);
		ret = cc1101_transmit_packet(cc, sync_packet,
					      sizeof(sync_packet));
		if (ret)
			goto out;
		if (i != CC1101_FHSS_SYNC_ACQUIRE_COUNT - 1)
			msleep(20);
	}

	/* slot 0의 실제 경계는 지금보다 guard만큼 뒤로 둔다. 바로 이어서
	 * 보내는 SYNC를 받은 SLAVE도 같은 계산으로 이 경계를 복원한다. */
	fhss->reference_time_ns = ktime_get_ns() +
		(u64)fhss->config.hop.channel_switch_guard_us * NSEC_PER_USEC;
	fhss->state = CC1101_FHSS_SYNCHRONIZED;
	kthread_queue_work(&fhss->worker, &fhss->hop_work);
out:
	mutex_unlock(&fhss->config_lock);
	return ret;
}

int cc1101_fhss_stop(struct cc1101 *cc)
{
	struct cc1101_fhss *fhss = cc->fhss;
	int ret = 0;

	if (!fhss)
		return -ENODEV;

	mutex_lock(&fhss->config_lock);
	if (fhss->state == CC1101_FHSS_DISABLED) {
		mutex_unlock(&fhss->config_lock);
		return 0;
	}
	fhss->state = CC1101_FHSS_STOPPING;
	mutex_unlock(&fhss->config_lock);

	/* 새 타이머 발생을 막고, 이미 대기 중이거나 실행 중인 작업도 기다려 끝낸다. */
	hrtimer_cancel(&fhss->timer);
	kthread_cancel_work_sync(&fhss->hop_work);

	/* 호핑을 끝낸 뒤에는 OTA/초기 접속에 쓰는 예약 채널로 돌아간다.
	 * 따라서 사용자 앱은 STOP 다음에 SET_CHANNEL을 따로 호출하지 않아도
	 * 다시 펌웨어 업데이트 패킷을 주고받을 수 있다. */
	mutex_lock(&cc->lock);
	ret = cc1101_switch_channel(cc, fhss->config.hop.reserved_channel);
	mutex_unlock(&cc->lock);

	mutex_lock(&fhss->config_lock);
	if (!ret)
		fhss->current_channel = fhss->config.hop.reserved_channel;
	else
		fhss->last_error = ret;
	fhss->state = fhss->algorithm ? CC1101_FHSS_CONFIGURED :
		CC1101_FHSS_DISABLED;
	mutex_unlock(&fhss->config_lock);
	return ret;
}

bool cc1101_fhss_is_sync_packet(struct cc1101 *cc,
				const u8 *payload, size_t len)
{
	struct cc1101_fhss *fhss = cc->fhss;
	enum cc1101_fhss_state state;

	if (!fhss || !payload || len != CC1101_FHSS_SYNC_PACKET_SIZE ||
	    payload[0] != CC1101_FHSS_SYNC_PACKET_TYPE ||
	    payload[1] != CC1101_FHSS_SYNC_VERSION)
		return false;

	/* IRQ 경로에서는 config_lock과 cc->lock의 순서를 뒤집지 않도록 상태를
	 * 읽기만 한다. 실제 검증과 상태 변경은 cc->lock을 푼 뒤 수행한다. */
	state = READ_ONCE(fhss->state);
	return READ_ONCE(fhss->role) == CC1101_FHSS_ROLE_SLAVE &&
		(state == CC1101_FHSS_SEARCHING ||
		 state == CC1101_FHSS_SYNCHRONIZED);
}

void cc1101_fhss_handle_sync(struct cc1101 *cc, const u8 *payload,
			     size_t len, u64 rx_time_ns)
{
	struct cc1101_fhss *fhss = cc->fhss;
	struct cc1101_fhss_sync sync;
	u64 slot_tmp, slot_ns, candidate_reference;
	u32 expected_index;
	u8 expected_channel;
	s64 error_ns;
	int ret;

	if (!fhss || cc1101_fhss_decode_sync(payload, len, &sync))
		return;

	mutex_lock(&fhss->config_lock);
	if (fhss->role != CC1101_FHSS_ROLE_SLAVE ||
	    (fhss->state != CC1101_FHSS_SEARCHING &&
	     fhss->state != CC1101_FHSS_SYNCHRONIZED))
		goto out;

	/* 예전 설정의 SYNC를 따라가면 모든 채널이 어긋나므로 generation이
	 * 같은 패킷만 인정한다. */
	if (sync.generation != fhss->config.generation)
		goto out;
	if (fhss->have_last_rx_sequence &&
	    sync.sequence == fhss->last_rx_sequence)
		goto out;

	slot_tmp = sync.slot_number;
	expected_index = do_div(slot_tmp, fhss->config.hop.channel_count);
	if (sync.hop_index != expected_index)
		goto out;
	ret = fhss->algorithm->channel_for_slot(fhss, sync.slot_number,
						&expected_channel);
	if (ret)
		goto out;
	if (fhss->state == CC1101_FHSS_SYNCHRONIZED &&
	    expected_channel != fhss->current_channel)
		goto out;

	slot_ns = (u64)fhss->config.hop.slot_duration_us * NSEC_PER_USEC;
	/* GDO0 패킷 종료 시각에는 짧은 무선 전송 시간이 포함된다. 다음 타이머를
	 * guard만큼 일찍 잡기 때문에 이 작은 오차 안에서도 채널을 미리 준비한다. */
	if ((u64)sync.slot_number * slot_ns > rx_time_ns)
		goto out;
	candidate_reference = rx_time_ns - (u64)sync.slot_number * slot_ns;
	fhss->sync_packets++;
	fhss->last_rx_sequence = sync.sequence;
	fhss->have_last_rx_sequence = true;
	fhss->last_sync_slot = sync.slot_number;
	fhss->sync_misses = 0;

	if (fhss->state == CC1101_FHSS_SEARCHING) {
		/* 우연히 한 번 잡힌 패킷으로 바로 호핑하지 않고 세 번 연속 같은
		 * generation의 SYNC를 확인해 잘못된 동기 획득을 줄인다. */
		fhss->acquire_progress++;
		if (fhss->acquire_progress < CC1101_FHSS_SYNC_ACQUIRE_COUNT)
			goto out;

		fhss->reference_time_ns = candidate_reference;
		fhss->current_slot = sync.slot_number;
		fhss->current_channel = expected_channel;
		fhss->state = CC1101_FHSS_SYNCHRONIZED;
		dev_info(&cc->spi->dev,
			 "FHSS SYNC 획득: generation=%u slot=%u channel=%u\n",
			 sync.generation, sync.slot_number, expected_channel);
		cc1101_fhss_schedule_next(fhss);
		goto out;
	}

	/* 이미 동기화된 뒤에는 한 번에 최대 500us만 보정한다. 순간적인 IRQ/SPI
	 * 지연 하나가 전체 기준 시각을 크게 밀어버리는 것을 막기 위한 제한이다. */
	error_ns = (s64)candidate_reference - (s64)fhss->reference_time_ns;
	if (error_ns > (s64)CC1101_FHSS_MAX_CORRECTION_US * NSEC_PER_USEC)
		error_ns = (s64)CC1101_FHSS_MAX_CORRECTION_US * NSEC_PER_USEC;
	else if (error_ns <
		 -(s64)CC1101_FHSS_MAX_CORRECTION_US * NSEC_PER_USEC)
		error_ns =
			-(s64)CC1101_FHSS_MAX_CORRECTION_US * NSEC_PER_USEC;
	if (error_ns < 0)
		fhss->reference_time_ns -= (u64)-error_ns;
	else
		fhss->reference_time_ns += error_ns;
out:
	mutex_unlock(&fhss->config_lock);
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
	status->enabled = fhss->state == CC1101_FHSS_SEARCHING ||
		fhss->state == CC1101_FHSS_SYNCHRONIZED;
	status->synchronized =
		fhss->state == CC1101_FHSS_SYNCHRONIZED;
	status->current_channel = fhss->current_channel;
	status->role = fhss->role;
	status->generation = fhss->config.generation;
	status->current_slot = fhss->current_slot;
	status->last_error = fhss->last_error;
	status->sync_misses = fhss->sync_misses;
	status->sync_packets = fhss->sync_packets;
	mutex_unlock(&fhss->config_lock);
}
