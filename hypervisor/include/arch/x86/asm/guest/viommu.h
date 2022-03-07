/*
 * Copyright (C) 2020 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VIOMMU_H
#define VIOMMU_H

struct acrn_viommu {
	spinlock_t lock;

	struct acrn_vm *vm;
	struct dmar_drhd_rt *drhd_rt;

	uint64_t qi_queue;
	uint16_t qi_head;
	uint16_t qi_tail;

	uint32_t gcmd;
};

void init_viommu(struct acrn_vm *vm);

#endif
