/*
 * Copyright (C) 2020 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#include <types.h>
#include <logmsg.h>
#include <asm/guest/vm.h>
#include <asm/vtd.h>
#include <asm/guest/ept.h>
#include <io_req.h>
#include <asm/guest/viommu.h>
#include <asm/board.h>

#define DBG_LEVEL_VIOMMU 5U

#define VTD_DOMAIN_ID_SHIFT             16  /* 16-bit domain id for 64K domains */
#define VTD_DOMAIN_ID_MASK              ((1UL << VTD_DOMAIN_ID_SHIFT) - 1)
#define VTD_INV_DESC_CC_G               (3ULL << 4)
#define VTD_INV_DESC_CC_GLOBAL          (1ULL << 4)
#define VTD_INV_DESC_CC_DOMAIN          (2ULL << 4)
#define VTD_INV_DESC_CC_DEVICE          (3ULL << 4)
#define VTD_INV_DESC_CC_DID(val)        (((val) >> 16) & VTD_DOMAIN_ID_MASK)
#define VTD_INV_DESC_CC_SID(val)        (((val) >> 32) & 0xffffUL)
#define VTD_INV_DESC_CC_FM(val)         (((val) >> 48) & 3UL)
#define VTD_INV_DESC_CC_RSVD            0xfffc00000000ffc0ULL

/* Masks for IOTLB Invalidate Descriptor */
#define VTD_INV_DESC_IOTLB_G            (3ULL << 4)
#define VTD_INV_DESC_IOTLB_GLOBAL       (1ULL << 4)
#define VTD_INV_DESC_IOTLB_DOMAIN       (2ULL << 4)
#define VTD_INV_DESC_IOTLB_PAGE         (3ULL << 4)
#define VTD_INV_DESC_IOTLB_DID(val)     (((val) >> 16) & VTD_DOMAIN_ID_MASK)
#define VTD_INV_DESC_IOTLB_ADDR(val)    ((val) & ~0xfffULL)
#define VTD_INV_DESC_IOTLB_AM(val)      ((val) & 0x3fULL)
#define VTD_INV_DESC_IOTLB_RSVD_LO      0xffffffff0000ff00ULL
#define VTD_INV_DESC_IOTLB_RSVD_HI      0xf80ULL



/* TODO: every DMAR in every guest should have one vIOMMU */
static struct acrn_viommu vdmar_drhd_units[MAX_DRHDS] = {0};

static void process_context_cache_desc(struct acrn_viommu *vdmar_unit, struct dmar_entry *entry)
{
	uint16_t sid = 0U, did = 0U;

	/* Figure 6-20. Context-cache Invalidate Descriptor */
	switch (entry->lo_64 & VTD_INV_DESC_CC_G) {
	case VTD_INV_DESC_CC_GLOBAL:
		/* On Linux, the translation table should be empty at this moment, just passthru this write */
		break;
	case VTD_INV_DESC_CC_DOMAIN:
		break;
	case VTD_INV_DESC_CC_DEVICE:
		entry->lo_64 = (entry->lo_64 & ~VTD_INV_DESC_CC_G) | VTD_INV_DESC_CC_DOMAIN;
		break;
	default:
		break;
	}
}

/* vt-d spec: 6.5.2.3 IOTLB Invalidate Descriptor */
static void process_iotlb_desc(struct acrn_viommu *vdmar_unit, struct dmar_entry *entry)
{
	uint64_t addr = 0UL, am = 0UL;
	uint16_t did = 0U;

	switch (entry->lo_64 & VTD_INV_DESC_IOTLB_G) {
	case VTD_INV_DESC_IOTLB_GLOBAL:
		break;
	case VTD_INV_DESC_IOTLB_DOMAIN:
		break;
	case VTD_INV_DESC_IOTLB_PAGE:
		if (!iommu_cap_max_amask_val(vdmar_unit->drhd_rt->cap)) {
			entry->lo_64 = (entry->lo_64 & ~VTD_INV_DESC_IOTLB_G) | VTD_INV_DESC_IOTLB_DOMAIN;
			entry->hi_64 = 0UL;
		}
		break;
	default:
		break;
	}
}

static void handle_iqt_register(struct acrn_viommu *vdmar_unit, uint16_t tail)
{
	struct dmar_drhd_rt *dmar_unit = vdmar_unit->drhd_rt;
	uint16_t head = vdmar_unit->qi_tail;	/* last tail from last write to iqt */
	struct dmar_entry *entry;
	bool write_iqt;

	stac();
	while (head != tail) {
		write_iqt = false;
		entry = (struct dmar_entry *)(vdmar_unit->qi_queue + head);

		switch (entry->lo_64 & DMAR_INV_DESC_MASK) {
		case DMAR_INV_CONTEXT_CACHE_DESC:
			//process_context_cache_desc(vdmar_unit, entry);
			write_iqt = true;
			break;
		case DMAR_INV_IOTLB_DESC:
			process_iotlb_desc(vdmar_unit, entry);
			write_iqt = true;
			break;
		case DMAR_INV_WAIT_DESC:
		{

			if (dmar_issue_qi_complete(dmar_unit)) {
				/* set the Done status in the wait entry */
				uint32_t *status_ptr = (uint32_t *)gpa2hva(vdmar_unit->vm, entry->hi_64);
				*status_ptr = (uint32_t)(entry->lo_64 >> 32U);

			}

#if 0
			if (dmar_unit->index == 0)
			//if (((entry->lo_64 >> 16U) & 0xffffU) == 0x13U)
			dev_dbg(DBG_LEVEL_VIOMMU, "vDMAR%d entry lo %llx hi 0x%llx",
				dmar_unit->index, entry->lo_64, entry->hi_64);
			//write_iqt = true;
#endif
			break;
		}

		default:
			pr_err("vDMAR%d entry lo %llx hi 0x%llx", dmar_unit->index, entry->lo_64, entry->hi_64);
			break;
		}

		if (write_iqt) {
#if 0
			if (dmar_unit->index == 0)
			//if (((entry->lo_64 >> 16U) & 0xffffU) == 0x13U)
			dev_dbg(DBG_LEVEL_VIOMMU, "vDMAR%d entry lo %llx hi %llx",
					dmar_unit->index, entry->lo_64, entry->hi_64);

#endif

			dmar_issue_qi_request(dmar_unit, *entry);
		}

		head = (head + DMAR_QI_INV_ENTRY_SIZE) % DMAR_INVALIDATION_QUEUE_SIZE;
	}
	clac();
}

static uint64_t viommu_mmio_read(struct acrn_viommu *vdmar_unit, struct acrn_mmio_request *mmio)
{
	struct dmar_drhd_rt *dmar_unit = vdmar_unit->drhd_rt;
	uint32_t offset = mmio->address - dmar_unit->drhd->reg_base_addr;
	uint64_t value;

	spinlock_obtain(&vdmar_unit->lock);

	switch (offset) {
	case DMAR_CAP_REG:
		pr_err("%s dmar%d cap: 0x%lx___", __func__, dmar_unit->index, dmar_unit->cap);
		/* Caching mode: In order to force Linux not to flush write buffer (__mapping_notify_one()) */
		value = (1UL << 7U);
		value |= (iommu_cap_sagaw(dmar_unit->cap) << 8U);
		value |= (iommu_cap_mgaw(dmar_unit->cap) << 16U);	/* Max Guest Address Width */
		value |= (iommu_cap_fault_reg_offset(dmar_unit->cap) << 24U);
		value |= ((0UL & 0xFFUL) << 40U);	/* NFR */
		value |= (dmar_unit->cap & 0x7UL); /* Number of Domains */
		value |= ((uint64_t)iommu_cap_max_amask_val(dmar_unit->cap) << 48U);
		value |= ((uint64_t)iommu_cap_pgsel_inv(dmar_unit->cap) << 39U);	/* page Selective Invalidation */
		value |= ((uint64_t)iommu_cap_super_page_val(dmar_unit->cap) << 34U); /* large page surpport */
		break;

	case DMAR_ECAP_REG:
		value = (1UL << 1U); 	/* Queue invalidation */
		break;

	case DMAR_IQT_REG:
		value = vdmar_unit->qi_tail;
		break;

	case DMAR_IQH_REG:
		value = vdmar_unit->qi_head;
		break;

	case DMAR_IQA_REG:
		value = vdmar_unit->qi_queue;
		break;

	default:
		if (mmio->size == 4U) {
			value = iommu_read32(dmar_unit, offset);
		} else {
			value = iommu_read64(dmar_unit, offset);
		}
	}

	spinlock_release(&vdmar_unit->lock);

	if ((offset != DMAR_FSTS_REG) || (value != 0U)) {
		dev_dbg(DBG_LEVEL_VIOMMU, "rd dmar%d offset %x size %x value %llx", dmar_unit->index, offset, mmio->size, value);
	}

	/* Remove Interrupt remapping Enabled flag */
	if (offset == DMAR_GSTS_REG) {
		value &= vdmar_unit->gcmd;
	}

	return value;
}

static void viommu_mmio_write(struct acrn_viommu *vdmar_unit, struct acrn_mmio_request *mmio)
{
	struct dmar_drhd_rt *dmar_unit = vdmar_unit->drhd_rt;
	uint32_t offset = mmio->address - dmar_unit->drhd->reg_base_addr;
	bool write_reg = true;

	if (offset != DMAR_IQT_REG) {
		dev_dbg(DBG_LEVEL_VIOMMU, "wr dmar%d offset %x size %x value %llx", dmar_unit->index, offset, mmio->size, mmio->value);
	}

	spinlock_obtain(&vdmar_unit->lock);

	switch (offset) {
	case DMAR_IQT_REG:
		if (vdmar_unit->gcmd & DMA_GCMD_QIE) {
			handle_iqt_register(vdmar_unit, mmio->value);
			vdmar_unit->qi_tail = (uint16_t)mmio->value;
			vdmar_unit->qi_head = vdmar_unit->qi_tail;
		} else {
			dev_dbg(DBG_LEVEL_VIOMMU, "%s %d_______________", __func__, __LINE__);
		}
		write_reg = false;
		break;

	case DMAR_IQA_REG:
		vdmar_unit->qi_queue = (uint64_t)gpa2hva(vdmar_unit->vm, mmio->value);
		vdmar_unit->qi_head = 0U;
		vdmar_unit->qi_tail = 0U;

		/* Don't write QI Addr register */
		write_reg = false;
		break;

	case DMAR_GCMD_REG:
	{
		uint32_t gsts = iommu_read32(dmar_unit, DMAR_GSTS_REG);
		vdmar_unit->gcmd = mmio->value;
		/* Reset SRTP (bit30) and TE (bits31) since we write through
		 * the Root Table Address Register (Register Offset 020h) now.
		 */
		mmio->value = gsts;

		dev_dbg(DBG_LEVEL_VIOMMU, "%s gsts: 0x%x val: 0x%x", __func__, gsts, mmio->value);
		break;
	}

	case DMAR_FECTL_REG:
	case DMAR_FEDATA_REG:
	case DMAR_FEADDR_REG:
	case DMAR_FEUADDR_REG:
		/* Hypervisor owns the fault management */
		write_reg = false;
		break;

	default:
		break;
	}

	if (write_reg) {
		if (mmio->size == 4U) {
			iommu_write32(dmar_unit, offset, (uint32_t)mmio->value);
		} else {
			iommu_write64(dmar_unit, offset, mmio->value);
		}
	}

	spinlock_release(&vdmar_unit->lock);

}

static int32_t viommu_mmio_handler(struct io_request *io_req, void *private_data)
{
	struct acrn_viommu *vdmar_unit = (struct acrn_viommu *)private_data;
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;

	if (mmio->direction == ACRN_IOREQ_DIR_READ) {
		mmio->value = viommu_mmio_read(vdmar_unit, mmio);
	} else {
		viommu_mmio_write(vdmar_unit, mmio);
	}

	return 0;
}

void init_viommu(struct acrn_vm *vm)
{
	uint32_t i;
	struct dmar_drhd_rt *dmar_unit = NULL;

	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		dmar_unit = get_drhd_unit(i);

		spinlock_init(&vdmar_drhd_units[i].lock);

		vdmar_drhd_units[i].drhd_rt = dmar_unit;

		/* Assuming one DMAR unit can be seen by one guest only */
		vdmar_drhd_units[i].vm = vm;

		register_mmio_emulation_handler(vm, viommu_mmio_handler,
			dmar_unit->drhd->reg_base_addr,
			dmar_unit->drhd->reg_base_addr + PAGE_SIZE,
			(void *)&vdmar_drhd_units[i], false);

		dev_dbg(DBG_LEVEL_VIOMMU, "register MMIO %llx", dmar_unit->drhd->reg_base_addr);
	}
}
