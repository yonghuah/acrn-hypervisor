/*
 * Copyright (C) 2020 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VIOMMU_H
#define VIOMMU_H

#define SHADOW_EN 1

#define MAX_GUEST_IOMMU_DID 128
struct acrn_viommu {
	spinlock_t lock;

	struct acrn_vm *vm;
	struct dmar_drhd_rt *drhd_rt;

	uint64_t qi_queue;
	uint16_t qi_head;
	uint16_t qi_tail;

	uint32_t gcmd;
#ifdef CONFIG_VIOMMU_ENABLED
	uint64_t guest_root_tbl_addr;
	uint8_t dmar_registers[4096];
	struct pgtable shadow_pgtable;
	uint64_t shadow_pml4[MAX_GUEST_IOMMU_DID];
	uint64_t guest_pml4_gpa[MAX_GUEST_IOMMU_DID];
	/*debug*/
	uint64_t map_cnt[MAX_GUEST_IOMMU_DID];
	uint64_t unmap_cnt[MAX_GUEST_IOMMU_DID];
#endif
};

void init_viommu(struct acrn_vm *vm);
void viommu_reserve_buffer_for_shadow_pages(void);

#endif
