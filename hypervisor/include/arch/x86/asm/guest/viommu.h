/*
 * Copyright (C) 2020 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VIOMMU_H
#define VIOMMU_H

#define SHADOW_EN 1

#define VTD_CAP_ESIRTPS		(1UL << 62)
#define VTD_CAP_FL5LP		(1UL << 60)
#define VTD_CAP_PI		(1UL << 59)
#define VTD_CAP_FL1GP		(1UL << 56)
#define VTD_CAP_DRD		(1UL << 55)
#define VTD_CAP_DWD		(1UL << 54)
#define VTD_CAP_PSI		(1UL << 39)
#define VTD_CAP_CM		(1UL << 7)
#define VTD_CAP_PHMR		(1UL << 6)
#define VTD_CAP_PLMR		(1UL << 5)
#define VTD_CAP_RWBF		(1UL << 4)
#define VTD_CAP_AFL		(1UL << 3)

#define VTD_ECAP_SC		(1UL << 7)
#define VTD_ECAP_DT		(1UL << 2)
#define VTD_ECAP_QI		(1UL << 1)
#define VTD_ECAP_C		(1UL << 0)  /* Page Walk Coherent */

#define IQ_QUEUE_QS_MASK	(0x7)
#define IQ_QUEUE_DW(iqa)	(((iqa) >> 11U) & 1UL)
#define IQ_INV_DESC_SIZE(dw)	((dw) ? 32U : 16U )
#define IQ_IQT_MASK(dw)		((dw) ? 0x3FFE0 : 0x7FFF0)

#define MAX_GUEST_IOMMU_DID 128
struct acrn_viommu {
	spinlock_t lock;

	struct acrn_vm *vm;
	struct dmar_drhd_rt *drhd_rt;

	uint64_t qi_queue;
	uint32_t qi_queue_size;
	uint32_t qi_dw;

#ifdef CONFIG_VIOMMU_ENABLED
	uint8_t regs[4096];
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
