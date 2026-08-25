#ifndef _CC1101_HOP_H_
#define _CC1101_HOP_H_

#include <linux/types.h>

struct cc1101_fhss;

struct cc1101_hop_algorithm {
	u32 id;
	const char *name;

	int (*init)(struct cc1101_fhss *fhss);
	int (*channel_for_slot)(struct cc1101_fhss *fhss, u64 slot,
				u8 *channel);
	void (*reset)(struct cc1101_fhss *fhss);
};

const struct cc1101_hop_algorithm *cc1101_hop_get_algorithm(u32 algorithm_id);

#endif /* _CC1101_HOP_H_ */
