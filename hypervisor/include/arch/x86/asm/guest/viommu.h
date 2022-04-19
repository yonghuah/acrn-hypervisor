/*
 * Copyright (C) 2020 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VIOMMU_H
#define VIOMMU_H


#define VIOMMU_DEBUG 1

#define MAX_GUEST_IOMMU_DID 128
struct acrn_viommu {
	spinlock_t lock;

	struct acrn_vm *vm;
	struct dmar_drhd_rt *drhd_rt;

	uint64_t qi_queue;
	uint32_t qi_queue_size;
	uint32_t qi_dw; /*todo*/
	uint32_t frcd_index; /*todo */
	uint32_t frcd_offset; /*todo*/

	uint8_t regs[PAGE_SIZE];
	struct pgtable shadow_pgtable;
	uint64_t shadow_pml4[MAX_GUEST_IOMMU_DID];
	uint64_t guest_pml4[MAX_GUEST_IOMMU_DID];

#if VIOMMU_DEBUG
	uint64_t map_cnt[MAX_GUEST_IOMMU_DID];
	uint64_t unmap_cnt[MAX_GUEST_IOMMU_DID];
#endif
};

void init_viommu(struct acrn_vm *vm);
void deinit_viommu(struct acrn_vm *vm);
void viommu_reserve_buffer_for_shadow_pages(void);
#endif
