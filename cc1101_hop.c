// SPDX-License-Identifier: GPL-2.0
/*
 * seed 값으로 채널 방문 순서를 만드는 파일이다.
 * 송신기와 수신기가 같은 seed와 같은 채널 범위를 사용하면 둘 다 똑같은
 * 순서를 만든다. 첫 채널은 동기화를 처음 찾는 랑데부 채널이므로 그대로 두고,
 * 두 번째 채널부터 섞는다. 이 규칙까지 같아야 ESP32와 같은 채널로 이동한다.
 */
#include <linux/errno.h>
#include <linux/math64.h>

#include "cc1101_fhss.h"
#include "cc1101_hop.h"

static u32 cc1101_xorshift32(u32 *state)
{
	u32 x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static int cc1101_seeded_init(struct cc1101_fhss *fhss)
{
	u16 count = fhss->config.hop.channel_count;
	u32 state = fhss->config.hop.seed ?:
		CC1101_FHSS_ZERO_SEED_FALLBACK;
	int i;

	if (!count || count > CC1101_FHSS_MAX_CHANNELS)
		return -EINVAL;

	/* 먼저 사용 가능한 채널을 순서대로 채운다. */
	for (i = 0; i < count; i++)
		fhss->permutation[i] = fhss->config.hop.first_channel + i;

	/* 0번은 랑데부 채널로 고정하고 1번 이후만 ESP32와 같은 방식으로 섞는다. */
	for (i = count; i > 2; i--) {
		u32 j = 1 + cc1101_xorshift32(&state) % (i - 1);
		u8 tmp = fhss->permutation[i - 1];

		fhss->permutation[i - 1] = fhss->permutation[j];
		fhss->permutation[j] = tmp;
	}

	return 0;
}

static int cc1101_seeded_channel_for_slot(struct cc1101_fhss *fhss,
					   u64 slot, u8 *channel)
{
	u16 count = fhss->config.hop.channel_count;
	u64 slot_tmp;
	u32 index;

	if (!count)
		return -EINVAL;

	/* 슬롯 번호를 채널 개수로 나눈 나머지가 이번에 방문할 순서다.
	 * 32비트 라즈베리파이 커널에서는 큰 정수의 일반 % 연산이 빌드 문제를
	 * 만들 수 있어서 커널이 제공하는 do_div()를 사용한다. */
	slot_tmp = slot;
	index = do_div(slot_tmp, count);
	*channel = fhss->permutation[index];
	return 0;
}

static void cc1101_seeded_reset(struct cc1101_fhss *fhss)
{
	fhss->current_slot = 0;
}

static const struct cc1101_hop_algorithm cc1101_seeded_ops = {
	.id = CC1101_FHSS_ALGORITHM_SEEDED_PERMUTATION,
	.name = "seeded-permutation",
	.init = cc1101_seeded_init,
	.channel_for_slot = cc1101_seeded_channel_for_slot,
	.reset = cc1101_seeded_reset,
};

const struct cc1101_hop_algorithm *cc1101_hop_get_algorithm(u32 algorithm_id)
{
	if (algorithm_id == CC1101_FHSS_ALGORITHM_SEEDED_PERMUTATION)
		return &cc1101_seeded_ops;

	return NULL;
}
