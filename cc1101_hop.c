// SPDX-License-Identifier: GPL-2.0
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
	u32 state = fhss->config.hop.seed ?: 1;
	int i;

	if (!count || count > CC1101_FHSS_MAX_CHANNELS)
		return -EINVAL;

	for (i = 0; i < count; i++)
		fhss->permutation[i] = fhss->config.hop.first_channel + i;

	for (i = count - 1; i > 0; i--) {
		u32 j = cc1101_xorshift32(&state) % (i + 1);
		u8 tmp = fhss->permutation[i];

		fhss->permutation[i] = fhss->permutation[j];
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

	/* 32비트 ARM 커널에서 일반 u64 나눗셈 런타임 심볼을 만들지 않는다. */
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
