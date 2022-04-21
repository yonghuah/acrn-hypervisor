/*
 * Copyright (C) 2020 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VIOMMU_H
#define VIOMMU_H


#define VIOMMU_DEBUG 1

/*
 * GUEST_IOMMU_CAP_ND means the max guest domain ID supported by vIOMMU,
 * Only below values are valid, and also this vaule must be configured
 * less than number of domains on physical DMAR.
 *
 * 0:  4-bit domain-ids with support for up to 16 domains.
 * 1:  6-bit domain-ids with support for up to 64 domains.
 * 2:  8-bit domain-ids with support for up to 256 domains.
 * 3:  10-bit domain-ids with support for up to 1024 domains.
 * 4:  12-bit domain-ids with support for up to 4K domains.
 * 5:  14-bit domain-ids with support for up to 16K domains.
 * 6:  16-bit domain-ids with support for up to 64K domains.
 * All other values are invalid.
 */
#define GUEST_IOMMU_CAP_ND	(1U)

/* Intel VT-d Specification 10.4.2 */
#define MAX_GUEST_IOMMU_DID ((1U) << (4U + (2U * (GUEST_IOMMU_CAP_ND & 0x7U))))

struct acrn_viommu {
	spinlock_t lock;

	struct acrn_vm *vm;
	struct dmar_drhd_rt *drhd_rt;

	/* Guest IQ base address */
	uint64_t qi_queue;

	/* Guest IQ size in bytes */
	uint32_t qi_queue_size;

	/* IQ Descriptor width, 0: 128-bit descriptors, 1: 256-bit descriptors */
	uint32_t qi_dw;

	/* Guest fault record index */
	uint32_t frcd_index;

	/* Guest fault recording register offset */
	uint32_t frcd_offset;

	/* vIOMMU register page */
	uint8_t regs[PAGE_SIZE];

	/* Shadow page table manangement structure */
	struct pgtable shadow_pgtable;

	/*Below two tables store pointers to shadow and guest page tables */
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
