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

#define DBG_LEVEL_VIOMMU 6U

/* TODO: every DMAR in every guest should have one vIOMMU */
static struct acrn_viommu vdmar_drhd_units[MAX_DRHDS] = {0};

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
			//process_iotlb_desc(vdmar_unit, entry);
			write_iqt = true;
			break;
		case DMAR_INV_WAIT_DESC:
		{
			/* set the Done status in the wait entry */
			uint32_t *status_ptr = (uint32_t *)gpa2hva(vdmar_unit->vm, entry->hi_64);
			*status_ptr = (uint32_t)(entry->lo_64 >> 32U);
			break;
		}

		default:
			dev_dbg(DBG_LEVEL_VIOMMU, "vDMAR%d entry lo %llx/%llx", dmar_unit->index, entry->lo_64, entry->hi_64);
			break;
		}

		if (write_iqt) {
			dev_dbg(DBG_LEVEL_VIOMMU, "vDMAR%d entry lo %llx hi %llx", dmar_unit->index, entry->lo_64, entry->hi_64);

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

	switch (offset) {
	case DMAR_CAP_REG:
		/* Caching mode: In order to force Linux not to flush write buffer (__mapping_notify_one()) */
		value = (1UL << 7U);
		value |= (iommu_cap_sagaw(dmar_unit->cap) << 8U);
		value |= (iommu_cap_mgaw(dmar_unit->cap) << 16U);	/* Max Guest Address Width */
		value |= (iommu_cap_fault_reg_offset(dmar_unit->cap) << 24U);
		value |= ((0UL & 0xFFUL) << 40U);	/* NFR */
		value |= (dmar_unit->cap & 0x7UL); /* Number of Domains */
		value |= ((uint64_t)iommu_cap_max_amask_val(dmar_unit->cap) << 48U);
		value |= (1UL << 39U);	/* page Selective Invalidation */
		value |= ((uint64_t)iommu_cap_super_page_val(dmar_unit->cap) << 34U); /* large page surpport */
		break;

	case DMAR_ECAP_REG:
		value = (1UL << 1U); 	/* Queue invalidation */
		value |= (1UL << 4U); 	/* Extented interrupt mode */
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

	if ((offset != DMAR_FSTS_REG) || (value != 0U)) {
		dev_dbg(DBG_LEVEL_VIOMMU, "rd dmar%d offset %x size %x value %llx", dmar_unit->index, offset, mmio->size, value);
	}

	/* Remove Interrupt remapping Enabled flag */
	if (offset == DMAR_GSTS_REG) {
		value &= vdmar_unit->gcmd;
		dev_dbg(DBG_LEVEL_VIOMMU, "rd2 dmar%d offset %x gcmd %lx, val: 0x%lx",
				dmar_unit->index, offset, vdmar_unit->gcmd, value);
	}

	return value;
}

static void viommu_mmio_write(struct acrn_viommu *vdmar_unit, struct acrn_mmio_request *mmio)
{
	struct dmar_drhd_rt *dmar_unit = vdmar_unit->drhd_rt;
	uint32_t offset = mmio->address - dmar_unit->drhd->reg_base_addr;
	bool write_reg = true;

	//if (offset != DMAR_IQT_REG) {
		dev_dbg(DBG_LEVEL_VIOMMU, "wr dmar%d offset %x size %x value %llx", dmar_unit->index, offset, mmio->size, mmio->value);
	//}

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

		mmio->value = ((mmio->value & ~(DMA_GCMD_SIRTP | DMA_GCMD_IRE)) | gsts);

		dev_dbg(DBG_LEVEL_VIOMMU, "%s gsts: 0x%x old: 0x%x new: 0x%x", __func__, gsts, vdmar_unit->gcmd, mmio->value);
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
