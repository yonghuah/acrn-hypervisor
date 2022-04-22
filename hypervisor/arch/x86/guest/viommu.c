/*
 * Copyright (C) 2020 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#include <types.h>
#include <logmsg.h>
#include <asm/guest/vm.h>
#include <asm/vtd.h>
#include <asm/mmu.h>
#include <asm/guest/ept.h>
#include <io_req.h>
#include <asm/guest/viommu.h>
#include <asm/board.h>


#define DBG_LEVEL_VIOMMU 5U
#if VIOMMU_DEBUG
#define CHECK_TIME 0
#endif

typedef uint64_t (*pgtable_mapping_handler)(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t gpa, uint64_t size, uint64_t permit);

/*
 * This pgtable instance is referecned only for guest page table entry lookup,
 * hence only 'pgentry_present_mask' is required and initialized, all other fileds
 * are initialized to NULL and shall never be accessed.
 */
static const struct pgtable guest_pgtable = {
	.pgentry_present_mask = EPT_RWX
};

/*
 * Below data structure to reserve memory pool for vIOMMU shadow page table:
 *  1) According to VT-d Specification section 9.3, IOMMU second level page table
 *     shall be shared between devices with the same Domain ID(DID). At the worst
 *     case, number of DID equals number of PCI devices, which is also the number
 *     of shadow page tables.
 *  2) Current support vIOMMU for service VM only.
 *  3) Define a global pool to hold all vIOMMU shadow tables, the number of which
 *     will be no greater than number of PCI devices.
 */
#define VIOMMU_MAX_SHADOW_NUM 16
#define VIOMMU_IOVA_SPACE_SIZE (MEM_2G)
#define VIOMMU_SHADOW_PML4_PAGE_NUM	PML4_PAGE_NUM(MAX_PHY_ADDRESS_SPACE)
#define VIOMMU_SHADOW_PDPT_PAGE_NUM	PDPT_PAGE_NUM(MAX_PHY_ADDRESS_SPACE)
static struct page *viommu_shadow_pages;
static uint64_t *viommu_shadow_page_bitmap;
static struct page viommu_shadow_dummy_pages;
static struct page_pool viommu_shadow_page_pool;

/*
 * Currently, support vIOMMU for serice VM only, need to move this definition
 * to struct acrn_vm when need to support vIOMMU for mulitple VM.
 */
static struct acrn_viommu viommu_units[MAX_DRHDS];

#if CHECK_TIME
#define RECORD_SHADOW_MAP	0
#define RECORD_SHADOW_UNMAP	1
#define RECORD_IOTLB		2
#define RECORD_MAX_TBL_NUM	3
static void insert_time(int tbl, uint64_t us);
#endif

static inline void shadow_clflush_pagewalk(const void* etry)
{
	iommu_flush_cache(etry, sizeof(uint64_t));
}

static inline bool shadow_large_page_support(enum _page_table_level level, __unused uint64_t prot)
{
	return ((level == IA32E_PD) || (level == IA32E_PDPT));
}

static inline void shadow_nop_tweak_exe_right(uint64_t *entry __attribute__((unused))) {}
static inline void shadow_nop_recover_exe_right(uint64_t *entry __attribute__((unused))) {}

static inline uint32_t viommu_read32(const struct acrn_viommu *viommu, uint32_t offset)
{
	return  *((uint32_t *)(viommu->regs + offset));
}

static inline uint64_t viommu_read64(const struct acrn_viommu *viommu, uint32_t offset)
{
	return *((uint64_t *)(viommu->regs + offset));
}

static inline void viommu_write32(const struct acrn_viommu *viommu, uint32_t offset, uint32_t value)
{
	*((uint32_t *)(viommu->regs + offset)) = value;
}

static inline void viommu_write64(const struct acrn_viommu *viommu, uint32_t offset, uint64_t value)
{
	*((uint64_t *)(viommu->regs + offset)) = value;
}

static inline bool is_leaf_entry(uint64_t entry)
{
	return ((entry & PAGE_PSE) != 0U);
}

static inline uint64_t get_guest_pml4(struct acrn_viommu *viommu, uint32_t did)
{
	return viommu->guest_pml4[did];
}

static inline uint64_t get_shadow_pml4(struct acrn_viommu *viommu, uint32_t did)
{
	return viommu->shadow_pml4[did];
}

/* get the Guest Root Table Address (HVA) */
static inline uint64_t get_guest_rta(struct acrn_viommu *viommu)
{
	return (uint64_t)gpa2hva(viommu->vm, viommu_read64(viommu, DMAR_RTADDR_REG) & PAGE_MASK);
}

static uint64_t get_shadow_page_num(void)
{
	uint64_t pd_page_num = PD_PAGE_NUM(VIOMMU_IOVA_SPACE_SIZE);
	uint64_t pt_page_num = PT_PAGE_NUM(VIOMMU_IOVA_SPACE_SIZE);

	return roundup((VIOMMU_SHADOW_PML4_PAGE_NUM + VIOMMU_SHADOW_PDPT_PAGE_NUM + pd_page_num + pt_page_num), 64U);
}

static void reserve_shadow_bitmap(void)
{
	uint64_t bitmap_base;
	uint64_t bitmap_size;

	bitmap_size = (get_shadow_page_num() * VIOMMU_MAX_SHADOW_NUM) / 8U;
	bitmap_base = e820_alloc_memory(bitmap_size, ~0UL);
	set_paging_supervisor(bitmap_base, bitmap_size);
	viommu_shadow_page_bitmap = (uint64_t *)bitmap_base;
}

static uint64_t get_total_shadow_4k_pages_size(void)
{
	return VIOMMU_MAX_SHADOW_NUM * (get_shadow_page_num()) * PAGE_SIZE;
}

/*
 * @brief Reserve space for 4K pages.
 */
void viommu_reserve_buffer_for_shadow_pages(void)
{
	uint64_t page_base;

	page_base = e820_alloc_memory(get_total_shadow_4k_pages_size(), ~0UL);
	set_paging_supervisor(page_base, get_total_shadow_4k_pages_size());
	viommu_shadow_pages = (struct page *)page_base;

	reserve_shadow_bitmap();
}

static void init_shadow_pgtable(struct acrn_viommu *viommu)
{
	static bool pool_init_done = false;
	struct pgtable *table;
	struct page_pool * pool;

	table = &viommu->shadow_pgtable;

	pool = &viommu_shadow_page_pool;
	if (!pool_init_done) {
		pool->start_page = viommu_shadow_pages;
		pool->bitmap_size = (VIOMMU_MAX_SHADOW_NUM * get_shadow_page_num()) / 64U;
		pool->bitmap = viommu_shadow_page_bitmap;
		pool->dummy_page = &viommu_shadow_dummy_pages;

		spinlock_init(&pool->lock);
		memset((void *)pool->bitmap, 0, pool->bitmap_size * sizeof(uint64_t));
		pool->last_hint_id = 0UL;
		pool_init_done = true;
	}

	table->pool = pool;
	table->default_access_right = EPT_RD | EPT_WR;
	table->pgentry_present_mask = EPT_RWX;
	table->clflush_pagewalk = shadow_clflush_pagewalk;
	table->large_page_support = shadow_large_page_support;
	table->tweak_exe_right = shadow_nop_tweak_exe_right;
	table->recover_exe_right = shadow_nop_recover_exe_right;
}

void viommu_shadow_add_mr(struct acrn_viommu *viommu, uint64_t *pml4_page,
	uint64_t hpa, uint64_t iova, uint64_t size, uint64_t prot)
{
	pgtable_add_map(pml4_page, hpa, iova, size, prot, &viommu->shadow_pgtable);
}

void viommu_shadow_del_mr(struct acrn_viommu *viommu, uint64_t *pml4_page, uint64_t iova, uint64_t size)
{
	pgtable_modify_or_del_map(pml4_page, iova, size, 0UL, 0UL, &(viommu->shadow_pgtable), MR_DEL);
}

/*
 * @brief Release all pages except the PML4E page of a shadow table 
 */
static void free_shadow_table(struct acrn_viommu *viommu, uint32_t guest_did)
{
	uint64_t *shadow_pml4e, *shadow_pdpte, *shadow_pde, *shadow_pml4;
	uint64_t i, j, k;
	struct pgtable *table;

	shadow_pml4 = (uint64_t *)viommu->shadow_pml4[guest_did];
	if (shadow_pml4) {
		table = &viommu->shadow_pgtable;
		for (i = 0UL; i < PTRS_PER_PML4E; i++) {
			shadow_pml4e = pml4e_offset(shadow_pml4, i << PML4E_SHIFT);
			if (!pgentry_present(table, (*shadow_pml4e))) {
				continue;
			}
			for (j = 0UL; j < PTRS_PER_PDPTE; j++) {
				shadow_pdpte = pdpte_offset(shadow_pml4e, j << PDPTE_SHIFT);
				if (!pgentry_present(table, (*shadow_pdpte)) ||
				    is_leaf_entry(*shadow_pdpte)) {
					continue;
				}
				for (k = 0UL; k < PTRS_PER_PDE; k++) {
					shadow_pde = pde_offset(shadow_pdpte, k << PDE_SHIFT);
					if (!pgentry_present(table, (*shadow_pde)) ||
					    is_leaf_entry(*shadow_pde)) {
						continue;
					}
					free_page(table->pool, (struct page *)((*shadow_pde) & EPT_ENTRY_PFN_MASK));
				}
				free_page(table->pool, (struct page *)((*shadow_pdpte) & EPT_ENTRY_PFN_MASK));
			}
			free_page(table->pool, (struct page *)((*shadow_pml4e) & EPT_ENTRY_PFN_MASK));
			*shadow_pml4e = 0UL;
		}
	}
}

static void delete_shadow_table(struct acrn_viommu *viommu, uint32_t guest_did)
{
	struct pgtable *table = &viommu->shadow_pgtable;

	free_shadow_table(viommu, guest_did);
	free_page(table->pool, (struct page *)viommu->shadow_pml4[guest_did]);
	viommu->shadow_pml4[guest_did] = 0UL;
}

static void create_shadow_table(struct acrn_viommu *viommu, uint32_t guest_did)
{
	/*
	 * VT-d specification #9.3, Context-entries programmed with the same domain identifier
	 * must always reference same address translation(SLPTPTR field), so shadow table is created
	 * for each IOMMU domain, instead of device specific.
	 */
	if (viommu->shadow_pml4[guest_did] == 0UL) {
		/* create IOMMU shadow table for this guest IOMMU domain */
		viommu->shadow_pml4[guest_did] = (uint64_t)pgtable_create_root(&viommu->shadow_pgtable);
	}

}

static uint64_t shadow_map_handler(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t gpa, uint64_t size, uint64_t permit)
{
	const uint64_t *shadow_pte;
	uint64_t hpa, pte_size, synced_size = size;
	uint64_t *shadow_pml4 = (uint64_t *)viommu->shadow_pml4[did];

	shadow_pte = pgtable_lookup_entry(shadow_pml4, iova, &pte_size, &guest_pgtable);
	if (permit != 0UL) { /* Add mapping to shadow table */
		if (shadow_pte) {
			/* corner case: mapping is already present in shadow table, remove it first */
			viommu_shadow_del_mr(viommu, shadow_pml4, iova, pte_size);
			synced_size = pte_size;
		}

		hpa = gpa2hpa(viommu->vm, gpa);
		if (hpa != INVALID_HPA) {
			viommu_shadow_add_mr(viommu, shadow_pml4, hpa, iova, size, permit);
		}

	}
	return synced_size;
}

/*@pre: shadow table shall be empty before doing this page table walk */
static void walk_guest_pgtable(struct acrn_viommu *viommu, uint16_t did, pgtable_mapping_handler shadow_map)
{
	uint64_t *pml4e, *pdpte, *pde, *pte;
	uint64_t i, j, k, m;
	uint64_t iova, gpa;
	const struct pgtable *table = &guest_pgtable;
	uint64_t guest_pml4 = get_guest_pml4(viommu, did);

	for (i = 0UL; i < PTRS_PER_PML4E; i++) {
		pml4e = pml4e_offset((uint64_t *)guest_pml4, i << PML4E_SHIFT);
		if (!pgentry_present(table, (*pml4e))) {
			continue;
		}
		for (j = 0UL; j < PTRS_PER_PDPTE; j++) {
			pdpte = pdpte_offset(pml4e, j << PDPTE_SHIFT);
			if (!pgentry_present(table, (*pdpte))) {
				continue;
			}
			if (pdpte_large(*pdpte) != 0UL) {
				iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT);
				gpa = (*pdpte & (~EPT_PFN_HIGH_MASK)) & (~(PDPTE_SIZE - 1UL));
				shadow_map(viommu, did, iova, gpa, PDPTE_SIZE, ((*pdpte) & EPT_RWX));
				continue;
			}
			for (k = 0UL; k < PTRS_PER_PDE; k++) {
				pde = pde_offset(pdpte, k << PDE_SHIFT);
				if (!pgentry_present(table, (*pde))) {
					continue;
				}
				if (pde_large(*pde) != 0UL) {
					iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT) | (k << PDE_SHIFT);
					gpa = (*pde & (~EPT_PFN_HIGH_MASK)) & (~(PDE_SIZE - 1UL));
					shadow_map(viommu, did, iova, gpa, PDE_SIZE, ((*pde) & EPT_RWX));
					continue;
				}
				for (m = 0UL; m < PTRS_PER_PTE; m++) {
					pte = pte_offset(pde, m << PTE_SHIFT);
					if (pgentry_present(table, (*pte))) {
						iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT) | (k << PDE_SHIFT) | (m << PTE_SHIFT);
						gpa = (*pte & (~EPT_PFN_HIGH_MASK)) & (~(PTE_SIZE - 1UL));
						shadow_map(viommu, did, iova, gpa, PTE_SIZE, ((*pte) & EPT_RWX));
					}
				}
			}
		}
	}
}

static void walk_guest_pgtable_range(struct acrn_viommu *viommu, uint16_t did, uint64_t addr, uint64_t size, pgtable_mapping_handler shadow_sync)
{
	uint64_t gpa, pte_size, req_size, synced_size, permit;
	const uint64_t *guest_pte;
	uint64_t iova_end = addr + size, iova = addr;
	uint64_t guest_pml4 = get_guest_pml4(viommu, did);
	uint64_t shadow_pml4 = get_shadow_pml4(viommu, did);

	while (iova < iova_end) {
		guest_pte = pgtable_lookup_entry((uint64_t *)guest_pml4, iova, &pte_size, &guest_pgtable);
		if (guest_pte == NULL) {
			synced_size = iova_end - iova;
			/* Remove mapping from shadow table */
			viommu_shadow_del_mr(viommu, (uint64_t *)shadow_pml4, iova, synced_size);
		} else {
			gpa = (((*guest_pte & (~EPT_PFN_HIGH_MASK)) & (~(pte_size - 1UL))) | (iova & (pte_size - 1UL)));
			permit = *guest_pte & EPT_RWX;
			req_size = (iova + pte_size <= iova_end) ? pte_size : iova_end - iova;
			/* Add mapping to shadow table */
			synced_size = shadow_sync(viommu, did, iova, gpa, req_size, permit);
		}
		iova += synced_size;
	}
}

static struct dmar_entry *get_shadow_context_entry(struct acrn_viommu *viommu, union pci_bdf *vbdf)
{
	uint32_t i;
	union pci_bdf pbdf;
	struct pci_vdev *vdev;
	struct acrn_vm *vm = viommu->vm;
	struct acrn_vpci *vpci = &(vm->vpci);
	struct dmar_entry *p_rta, *p_root_e, *p_context, *p_context_e = NULL;

	p_rta = (struct dmar_entry *)viommu->drhd_rt->root_table_addr;
	for (i = 0; i < vpci->pci_vdev_cnt; i++) {
		vdev =&(vpci->pci_vdevs[i]);
		if ((vbdf->bits.b == vdev->bdf.bits.b) &&
				(vbdf->bits.d == vdev->bdf.bits.d) &&
				(vbdf->bits.f == vdev->bdf.bits.f)) {

			memcpy_s(&pbdf, sizeof(union pci_bdf), &(vdev->pdev->bdf), sizeof(union pci_bdf));

			p_root_e = p_rta + pbdf.fields.bus;
			ASSERT(((p_root_e->lo_64 & 0x1) == 1), "Invalid Root Entry.");

			p_context = (struct dmar_entry *)(p_root_e->lo_64 & PAGE_MASK);

			p_context_e = p_context + pbdf.fields.devfun;
			break;
		}
	}

	ASSERT(p_context_e != NULL, "Not found context entry.");
	return p_context_e;
}

/*
 * Reserve DID number for hypervisor usage, as support vIOMMU for service VM
 * only for now, so we can ensure the domain ID is global unique in native
 * IOMMU context table after this domain ID remapping.
 */
#define HV_RSV_DID_NUM 8U
static uint32_t remap_did(__unused struct acrn_viommu *viommu, uint32_t guest_did)
{
	return (guest_did + HV_RSV_DID_NUM);
}

static int context_cache_inv_device(struct acrn_viommu *viommu, __unused uint32_t cc_did, uint32_t sid, uint32_t fm)
{
	int status = -1;
	union pci_bdf vbdf;
	uint16_t bus, devfun, did, remapped_did;
	uint64_t shadow_pml4, guest_pml4;
	struct dmar_entry *guest_root_e;
	struct dmar_entry *p_guest_context_e, *p_shadow_context_e;

	if (fm != 0U) {
		pr_fatal("%s, Can't support function mask.", __func__);
	}

	bus = (sid >> 8) & 0xFF;
	devfun = sid & 0xFF;
	guest_root_e = (struct dmar_entry *)get_guest_rta(viommu) + bus;
	if (dmar_get_bitslice(guest_root_e->lo_64, ROOT_ENTRY_LOWER_PRESENT_MASK, ROOT_ENTRY_LOWER_PRESENT_POS) != 0UL) {
		p_guest_context_e = (struct dmar_entry *)gpa2hva(viommu->vm, guest_root_e->lo_64 & PAGE_MASK) + devfun;
		if (dmar_get_bitslice(p_guest_context_e->lo_64, CTX_ENTRY_LOWER_P_MASK, CTX_ENTRY_LOWER_P_POS) != 0UL) {
			did = dmar_get_bitslice(p_guest_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);
			ASSERT(did < MAX_GUEST_IOMMU_DID, "Guest DID overflow");

			guest_pml4 = p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
			ASSERT(guest_pml4 != 0UL, "");/*Todo: inject error to guest. */

			if (viommu->guest_pml4[did] == 0UL) {
				viommu->guest_pml4[did] = (uint64_t)gpa2hva(viommu->vm, guest_pml4);
			} else if (((uint64_t)gpa2hva(viommu->vm, guest_pml4)) != viommu->guest_pml4[did]) {
				 /* Guest is trying to invalidate page table for current domain */
				pr_err("DMAR%d, DID: Invalidate guest page table: %llx", viommu->drhd_rt->index, did);
				free_shadow_table(viommu, did);
				viommu->guest_pml4[did] = guest_pml4;
			}

			vbdf.fields.bus = bus;
			vbdf.fields.devfun = devfun;
			p_shadow_context_e = get_shadow_context_entry(viommu, &vbdf);
			if(p_shadow_context_e != NULL) {
				create_shadow_table(viommu, did);
				/* update shadow context entry */
				shadow_pml4 = viommu->shadow_pml4[did];
				p_shadow_context_e->lo_64 = p_guest_context_e->lo_64;
				p_shadow_context_e->lo_64 &= (~CTX_ENTRY_LOWER_SLPTPTR_MASK);
				#if 1 /*use shadow page table*/
				p_shadow_context_e->lo_64 |= (shadow_pml4 & CTX_ENTRY_LOWER_SLPTPTR_MASK);
				#else /*use guest page table directly for debug purpose only.*/
				p_shadow_context_e->lo_64 |= (guest_pml4 & CTX_ENTRY_LOWER_SLPTPTR_MASK);
				#endif

				remapped_did = remap_did(viommu, did);
				p_shadow_context_e->hi_64 = p_guest_context_e->hi_64;
				p_shadow_context_e->hi_64 &= (~CTX_ENTRY_UPPER_DID_MASK);
				p_shadow_context_e->hi_64 |= ((remapped_did << CTX_ENTRY_UPPER_DID_POS) & CTX_ENTRY_UPPER_DID_MASK);

				iommu_flush_cache(p_shadow_context_e, sizeof(struct dmar_entry));
				status = 0;
			}
		}
	}
	return status;
}

static int context_cache_inv_global(struct acrn_viommu *viommu, uint32_t fm)
{
	uint16_t bus, devfun;
	uint32_t did, sid;
	struct dmar_entry *root_entry, *ctp, *context_e;

	/* delete all shadow tables in current vIOMMU scope. */
	for (did = 0; did < MAX_GUEST_IOMMU_DID; did++) {
		if (viommu->shadow_pml4[did] != 0UL) {
			pr_err("%s, Shadow Delete: DMAR%d, did:%d.", __func__, viommu->drhd_rt->index, did);
			delete_shadow_table(viommu, did);
		}
	}

	/* reset all previous stored guest PML4 pointers */
	memset((void *)&viommu->guest_pml4[0], 0, sizeof(uint64_t) * (MAX_GUEST_IOMMU_DID));

	/*
	 * walk through guest root table, for Linux guest case, all context entries
	 * in root table are invalid when doing CC_Global.
	 */
	root_entry = (struct dmar_entry *)(get_guest_rta(viommu));
	for (bus = 0; bus < 256; bus++) {
		if (dmar_get_bitslice(root_entry[bus].lo_64, ROOT_ENTRY_LOWER_PRESENT_MASK,
			ROOT_ENTRY_LOWER_PRESENT_POS) != 0UL) {
			ctp = (struct dmar_entry *)gpa2hva(viommu->vm, root_entry[bus].lo_64 & PAGE_MASK);
			for (devfun = 0; devfun < 256; devfun++) {
				context_e = &ctp[devfun];
				if (dmar_get_bitslice(context_e->lo_64, CTX_ENTRY_LOWER_P_MASK,
					CTX_ENTRY_LOWER_P_POS) != 0UL) {
					did = dmar_get_bitslice(context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK,
						CTX_ENTRY_UPPER_DID_POS);
					ASSERT(did < MAX_GUEST_IOMMU_DID, "Guest DID overflow");

					sid = (bus << 8) | devfun;
					context_cache_inv_device(viommu, did, sid, fm);

				}
			}
		}
	}
	return 0;
}

static int iotlb_inv_psi(struct acrn_viommu *viommu, struct dmar_entry *iotlb_inv_desc)
{
	uint64_t did, am, addr, size;
	int status = -1;
	uint64_t guest_pml4;
	uint64_t shadow_pml4;

	did = DMAR_INV_DESC_IOTLB_DID(iotlb_inv_desc->lo_64);
	addr = DMAR_INV_DESC_IOTLB_ADDR(iotlb_inv_desc->hi_64);
	am = DMAR_INV_DESC_IOTLB_AM(iotlb_inv_desc->hi_64);
	size = ((1 << am) << 12);

	if (did >= MAX_GUEST_IOMMU_DID) {
		pr_err("%s, Can't support guest did:%d.\n", __func__, did);
		return -1;
	}

	guest_pml4 = get_guest_pml4(viommu, (uint32_t)did);
	shadow_pml4 = get_shadow_pml4(viommu, (uint32_t)did);
	if ((guest_pml4 != 0UL) && (shadow_pml4 != 0UL)) {
		walk_guest_pgtable_range(viommu, did, addr, size, shadow_map_handler);
	}

	return status;
}

static int iotlb_inv_domain(struct acrn_viommu *viommu, uint32_t did)
{
	uint64_t guest_pml4, shadow_pml4;

	guest_pml4 = get_guest_pml4(viommu, did);
	shadow_pml4 = get_shadow_pml4(viommu, did);

	if ((guest_pml4 != 0UL) && (shadow_pml4 != 0UL)) {
		free_shadow_table(viommu, did);
		walk_guest_pgtable(viommu, did, shadow_map_handler);
	}
	return 0;
}

static int iotlb_inv_global(struct acrn_viommu *viommu)
{
	uint32_t did;

	for (did = 0; did < MAX_GUEST_IOMMU_DID; did++) {
		if ((get_guest_pml4(viommu, did) == 0UL)
			&& (get_shadow_pml4(viommu, did) == 0UL)) {
			continue;
		}
		iotlb_inv_domain(viommu, did);
	}
	return 0;
}

static int process_context_cache_desc(struct acrn_viommu *viommu, struct dmar_entry *entry)
{
	int status = -1;
	uint32_t sid = 0U, did = 0U, fm = 0;
	uint64_t cc_g = entry->lo_64 & DMAR_INV_DESC_CC_G;

	fm = DMAR_INV_DESC_CC_FM(entry->lo_64);
	switch (cc_g) {
	case DMAR_INV_DESC_CC_GLOBAL:
		/* On Linux, the translation table is empty at this moment, just passthru this write */
		status =  context_cache_inv_global(viommu, fm);
		break;

	case DMAR_INV_DESC_CC_DOMAIN:
		break;

	case DMAR_INV_DESC_CC_DEVICE:
		did = DMAR_INV_DESC_CC_DID(entry->lo_64); /* always be 0 from linux guest. */
		sid = DMAR_INV_DESC_CC_SID(entry->lo_64);
		status = context_cache_inv_device(viommu, did, sid, fm);
		break;

	default:
		break;
	}

	return status;
}

static void remap_iotlb_desc_did(struct acrn_viommu *viommu, struct dmar_entry *iotlb_entry, uint32_t guest_did)
{
	uint32_t remapped_did = remap_did(viommu, guest_did);

	remapped_did &= 0xFFFF; /* Max bit width of DID is 16 */
	iotlb_entry->lo_64 &= ~(IOTLB_INV_LOWER_DID_MASK);
	iotlb_entry->lo_64 |= (remapped_did << IOTLB_INV_LOWER_DID_POS);
}

/* vt-d spec: 6.5.2.3 IOTLB Invalidate Descriptor */
static bool process_iotlb_desc(struct acrn_viommu *viommu, struct dmar_entry *entry)
{
	bool write_iqt = true;
	uint32_t did;
	struct dmar_entry iotlb_desc;
	int index = viommu->drhd_rt->index;

	did = DMAR_INV_DESC_IOTLB_DID(entry->lo_64);
	switch (entry->lo_64 & IOTLB_INV_LOWER_G_MASK) {
	case DMAR_INV_DESC_IOTLB_GLOBAL:
		iotlb_inv_global(viommu);
		break;

	case DMAR_INV_DESC_IOTLB_DOMAIN:
		/*guest page table maybe present when guest issue domain iotlb.*/
		iotlb_inv_domain(viommu, did);
		remap_iotlb_desc_did(viommu, entry, did);
		break;

	case DMAR_INV_DESC_IOTLB_PAGE:
		if (!iommu_cap_max_amask_val(viommu->drhd_rt->cap)) {
			entry->lo_64 = (entry->lo_64 & ~IOTLB_INV_LOWER_G_MASK) | DMAR_INV_DESC_IOTLB_DOMAIN;
			entry->hi_64 = 0UL;
		}

		iotlb_inv_psi(viommu, entry);
		remap_iotlb_desc_did(viommu, entry, did);
		if (!(viommu->drhd_rt->cap & DMAR_CAP_PSI)) { /* No PSI support on host */
			pr_err("%s, DMAR%d, IOTLB_PSI(Not support Natively), did:%d, iova:0x%llx, pages:%d.",
				__func__, index, (entry->lo_64 >> 16) & 0xFFFF, entry->hi_64 & (~0xfff), 1 << (entry->hi_64 & 0x3f));

			/* fallback to domain iotlb flush */
			iotlb_desc.lo_64 = DMA_IOTLB_DR | DMA_IOTLB_DW | DMA_IOTLB_DOMAIN_INVL| DMAR_INV_IOTLB_DESC;
			iotlb_desc.lo_64 |= (entry->lo_64 & IOTLB_INV_LOWER_DID_MASK);
			iotlb_desc.hi_64 = 0UL;
			remap_iotlb_desc_did(viommu, &iotlb_desc, did);
			dmar_issue_qi_request(viommu->drhd_rt, iotlb_desc);
			write_iqt = false; /* caller does not need to issue more IQ request for this flush.*/
		}
		break;

	default:
		break;
	}

	return write_iqt;
}

#define FSTS_RW1CS_BITS (DMAR_FSTS_PFO | DMAR_FSTS_IQE | DMAR_FSTS_ICE | DMAR_FSTS_ITE)
static int handle_fsts_write(struct acrn_viommu *viommu, uint32_t req_fsts)
{
	uint32_t fsts = viommu_read32(viommu, DMAR_FSTS_REG);
	uint32_t rw1cs_bits = req_fsts & FSTS_RW1CS_BITS;

	if (req_fsts & DMAR_FSTS_PFO) {
		/*reset FRI*/
		fsts &= ~DMAR_FSTS_FRI_MASK;
	}
	fsts &= ~rw1cs_bits;
	viommu_write32(viommu, DMAR_FSTS_REG, fsts);

	return 0;
}

static int handle_fectl_write(struct acrn_viommu *viommu, uint32_t req_fectl)
{
	uint32_t fectl = viommu_read32(viommu, DMAR_FECTL_REG);
	uint32_t req_bits = req_fectl ^ fectl;

	/* only IM(bit31) can be set */
	if ((req_bits & DMAR_FECTL_IM_MASK) != 0U) {
		if (req_fectl & DMAR_FECTL_IM_MASK) {
			if ((fectl & DMAR_FECTL_IM_MASK) == 0U) {
				/*Set IM*/
				fectl |= (DMAR_FECTL_IM_MASK);
				viommu_write32(viommu, DMAR_FECTL_REG, fectl);
			}
		} else {
			if ((fectl & DMAR_FECTL_IM_MASK) != 0U) {
				/*Clear IM*/
				fectl &= (~DMAR_FECTL_IM_MASK);
				viommu_write32(viommu, DMAR_FECTL_REG, fectl);
				if (fectl & DMAR_FECTL_IP_MASK) {
					/* Todo: report potential pending fault event */
				}
			}
		}
	}

	return 0;
}

static void handle_iqt_write(struct acrn_viommu *viommu, uint16_t tail)
{
	bool write_iqt;
	uint64_t head;
	uint32_t *status_ptr;
	struct dmar_entry *entry;
	struct dmar_drhd_rt *dmar_unit = viommu->drhd_rt;

	head = viommu_read64(viommu, DMAR_IQT_REG) & IQ_IQT_MASK(viommu->qi_dw);
	stac();
	while (head != tail) {
		write_iqt = false;
		entry = (struct dmar_entry *)(viommu->qi_queue + head);

		switch (entry->lo_64 & DMAR_INV_DESC_MASK) {
		case DMAR_INV_CONTEXT_CACHE_DESC:
			process_context_cache_desc(viommu, entry);
			write_iqt = true; 
			break;

		case DMAR_INV_IOTLB_DESC:
			write_iqt = process_iotlb_desc(viommu, entry);
			break;

		case DMAR_INV_WAIT_DESC:
		{
			/* Currently, Invalidation Completion Event is not supported */
			if ((entry->lo_64 & DMAR_INV_WAIT_IF) == 0UL) {
				if (dmar_issue_qi_complete(dmar_unit)) {
					/* set the Done Status in the wait entry */
					status_ptr = (uint32_t *)gpa2hva(viommu->vm, (entry->hi_64 & (~0x3UL)));
					if (status_ptr != NULL) {
						*status_ptr = (uint32_t)(entry->lo_64 >> 32U);
					} else {
						pr_fatal("%s, Invalid Status Address(GPA):%llx", __func__, entry->hi_64);
					}
				}
			} else {
				pr_fatal("%s, Invalidation Completion Event is not supported yet.", __func__);
			}

			break;
		}

		default:
			pr_err("Unhandled IQ request: vDMAR%d entry lo %llx hi 0x%llx", dmar_unit->index, entry->lo_64, entry->hi_64);
			break;
		}

		if (write_iqt) {
			dmar_issue_qi_request(dmar_unit, *entry);
		}

		head = (head + IQ_INV_DESC_SIZE(viommu->qi_dw)) % viommu->qi_queue_size;
	}
	clac();
}

static int handle_gcmd(struct acrn_viommu *viommu)
{
#define UNSUPPORTED_GCMD (DMA_GCMD_CFI | DMA_GCMD_IRE | DMA_GCMD_SIRTP | DMA_GCMD_EAFL | DMA_GCMD_SFL)
	uint32_t gcmd, req_bits, v_gsts = 0U;

	gcmd = viommu_read32(viommu, DMAR_GCMD_REG);
	v_gsts = viommu_read32(viommu, DMAR_GSTS_REG);
	req_bits = v_gsts ^ gcmd;

	ASSERT(((req_bits & UNSUPPORTED_GCMD) == 0U), "unsupported GCMD bits.");

	if (req_bits & DMA_GCMD_TE) {
		if (gcmd & DMA_GCMD_TE) {
			v_gsts |= DMA_GSTS_TES;
		} else {
			v_gsts &= (~DMA_GSTS_TES);
		}
	}

	if (req_bits & DMA_GCMD_QIE) {
		if (gcmd & DMA_GCMD_QIE) {
			v_gsts |= DMA_GSTS_QIES;
		} else {
			v_gsts &= (~DMA_GSTS_QIES);
		}
	}

	if (req_bits & DMA_GCMD_SRTP) {
		v_gsts |= DMA_GSTS_RTPS;
	}

	if (req_bits & DMA_GCMD_WBF) {
		if (gcmd & DMA_GCMD_WBF) {
			v_gsts |=  DMA_GSTS_WBFS;
		} else {
			v_gsts &= (~DMA_GSTS_WBFS);
		}
	}

	viommu_write32(viommu, DMAR_GSTS_REG, v_gsts);
	return 0;
}

#if VIOMMU_DEBUG
#define EMUL_TBL_NUM 32U
static uint32_t emulated_regs[EMUL_TBL_NUM] = {
	DMAR_VER_REG,
	DMAR_CAP_REG,
	DMAR_ECAP_REG,
	DMAR_GSTS_REG,
	DMAR_FSTS_REG,
	DMAR_FECTL_REG,
	DMAR_IQT_REG,
	DMAR_IQH_REG,
	DMAR_IQA_REG
};

static bool is_emulated_access(uint32_t offset)
{
	uint32_t i;

	for (i = 0; i < EMUL_TBL_NUM; i++) {
		if (emulated_regs[i] == offset)
			return true;
	}
	return false;
}
#endif

#define MAX_DMAR_REG_SPACE 0x1000
static uint64_t viommu_mmio_read(struct acrn_viommu *viommu, struct acrn_mmio_request *mmio)
{
	uint64_t value = 0UL;
	uint32_t offset = mmio->address - viommu->drhd_rt->drhd->reg_base_addr;

	if (offset + mmio->size <= MAX_DMAR_REG_SPACE) {
#if VIOMMU_DEBUG
		if (!is_emulated_access(offset)) {
			pr_err("%s, WARNING: offset:%x is NOT emulated yet!", __func__, offset);
		}
#endif
		spinlock_obtain(&viommu->lock);
		if (mmio->size == 4U) {
			value = viommu_read32(viommu, offset);
		} else if (mmio->size == 8U) {
			value = viommu_read64(viommu, offset);
		}
		spinlock_release(&viommu->lock);
	} else {
		pr_err("%s, Error: offset:%x overflow!", __func__, offset);
	}

	return value;
}

static void viommu_mmio_write(struct acrn_viommu *viommu, struct acrn_mmio_request *mmio)
{
	uint32_t v_gsts;
	uint64_t iq_addr;
	int index = viommu->drhd_rt->index;
	uint32_t offset = mmio->address - viommu->drhd_rt->drhd->reg_base_addr;

	spinlock_obtain(&viommu->lock);
	switch (offset) {
	case DMAR_GCMD_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		handle_gcmd(viommu);
		break;

	case DMAR_RTADDR_REG:
		if (RTA_TTM(mmio->value) == TTM_LEGACY_MODE) {
			viommu_write64(viommu, offset, mmio->value);
		} else {
			pr_fatal("%s, Support Legacy Mode Only.", __func__);
		}
		break;

	case DMAR_FSTS_REG:
		handle_fsts_write(viommu, mmio->value);
		break;

	case DMAR_FECTL_REG:
		handle_fectl_write(viommu, mmio->value);
		break;

	case DMAR_FEDATA_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		break;

	case DMAR_FEADDR_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		break;

	case DMAR_FEUADDR_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		break;

	case DMAR_IQT_REG:
		v_gsts = viommu_read32(viommu, DMAR_GSTS_REG);
		if (v_gsts & DMA_GSTS_QIES) {
			handle_iqt_write(viommu, mmio->value);

			/* update guest IQT & IQH */
			viommu_write64(viommu, DMAR_IQT_REG, mmio->value);
			viommu_write64(viommu, DMAR_IQH_REG, mmio->value);
		}
		break;

	case DMAR_IQA_REG:
		iq_addr = mmio->value;
		viommu_write64(viommu, DMAR_IQA_REG, iq_addr);

		viommu->qi_queue = (uint64_t)gpa2hva(viommu->vm, iq_addr);
		viommu->qi_dw= IQ_QUEUE_DW(iq_addr);
		viommu->qi_queue_size = (PAGE_SIZE) << (iq_addr & IQ_QUEUE_QS_MASK);
		viommu_write64(viommu, DMAR_IQH_REG, 0UL);
		viommu_write64(viommu, DMAR_IQT_REG, 0UL);

		break;

	default:
		pr_err("%s, DMAR%d, ERROR: Unhandled Write offset:0x%x, value:0x%llx", __func__, index, offset, mmio->value);
		break;
	}
	spinlock_release(&viommu->lock);
}

static int32_t viommu_mmio_handler(struct io_request *io_req, void *private_data)
{
	struct acrn_viommu *viommu = (struct acrn_viommu *)private_data;
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;

	if (mmio->direction == ACRN_IOREQ_DIR_READ) {
		mmio->value = viommu_mmio_read(viommu, mmio);
	} else {
		viommu_mmio_write(viommu, mmio);
	}

	return 0;
}

static void init_viommu_registers(struct acrn_viommu *viommu)
{
#define CAP_RSV_BITS_MASK ((7UL << 13U) | (1UL << 38U) | (3UL << 57) | (1UL << 61U))
	uint64_t val64;
	struct dmar_drhd_rt *dmar_unit = viommu->drhd_rt;

	/* version */
	viommu_write32(viommu, DMAR_VER_REG, iommu_read32(dmar_unit, DMAR_VER_REG));

	/* capability */
	val64 = dmar_unit->cap;
	 /* Always clear bits. */
	val64 &= (~(DMAR_CAP_ESIRTPS | DMAR_CAP_FL5LP | DMAR_CAP_PI | DMAR_CAP_FL1GP | DMAR_CAP_PHMR | DMAR_CAP_PLMR | DMAR_CAP_AFL));
	val64 &= (~CAP_RSV_BITS_MASK); /*clear reserve bits*/
	val64 |= (DMAR_CAP_PSI | DMAR_CAP_CM); /* Always set capability bits */

	/*
	 * number of physial IOMMU domains must be greater than hardcoded number
	 * for guest, as some domain-id need to be reserved for hypervisor native usage.
	 */
	if (iommu_cap_ndoms(dmar_unit->cap) > iommu_cap_ndoms(GUEST_IOMMU_CAP_ND)) {
		val64 &= (~DMAR_CAP_ND_MASK);
		val64 |= (GUEST_IOMMU_CAP_ND & 0x7U);
	} else {
		panic("Native IOMMU domain-id space is not enough, please tune GUEST_IOMMU_CAP_ND.");
	}

	/* set number of fault registers */
	val64 = dmar_set_bitslice(val64, DMAR_CAP_NFR_MASK, DMAR_CAP_NFR_POS, DMAR_FCRD_REG_NR - 1U);
	viommu->frcd_index = 0U;
	viommu->frcd_offset = dmar_get_bitslice(val64, DMAR_CAP_FRO_MASK, DMAR_CAP_FRO_POS);

	viommu_write64(viommu, DMAR_CAP_REG, val64);

	/* extend Capability */
	val64 = dmar_unit->ecap;
	/* expose below extend capability bits only */
	val64 &= (DMAR_ECAP_SC | DMAR_ECAP_DT | DMAR_ECAP_QI | DMAR_ECAP_C);
	viommu_write64(viommu, DMAR_ECAP_REG, val64);
	/*pr_err("%s, DMAR%d: Host ecap: %-16llx Guest ecap: %-16llx", __func__,
		index, dmar_unit->ecap, viommu_read64(viommu, DMAR_ECAP_REG));*/

	/* initialize FSTS */
	viommu_write32(viommu, DMAR_FSTS_REG, 0U);
	viommu_write32(viommu, DMAR_FECTL_REG, DMAR_FECTL_IM_MASK);
}

void init_viommu(struct acrn_vm *vm)
{
	uint32_t i;
	struct dmar_drhd_rt *dmar_unit = NULL;

	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		dmar_unit = get_drhd_unit(i);

		spinlock_init(&viommu_units[i].lock);

		viommu_units[i].drhd_rt = dmar_unit;

		viommu_units[i].vm = vm;

		init_viommu_registers(&viommu_units[i]);

		init_shadow_pgtable(&viommu_units[i]);

		/*
		 * Currently, ACRN supports vIOMMU for service VM only, which can detect
		 * IOMMU by parsing native ACPI table, hence need to keep identical IOMMU
		 * register address base.
		 */
		register_mmio_emulation_handler(vm, viommu_mmio_handler,
			dmar_unit->drhd->reg_base_addr,
			dmar_unit->drhd->reg_base_addr + PAGE_SIZE,
			(void *)&viommu_units[i], false);
	}
}

void deinit_viommu(__unused struct acrn_vm *vm)
{
	uint32_t i, j;
	struct acrn_viommu *viommu;

	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		viommu = &viommu_units[i];
		for (j = 0U; j < MAX_GUEST_IOMMU_DID; j++) {
			if (viommu->shadow_pml4[j] != 0UL) {
				delete_shadow_table(viommu, j);
			}
		}
	}
}

#if VIOMMU_DEBUG
/*All below are debug code */
static void dump_root_entry(char *str, int bus, struct dmar_entry * p_root_e)
{
	pr_err("%s: root entry[bus = %d], conext pointer:%llx.", str, bus, p_root_e->lo_64);
}

static int dump_context_entry(char *str, uint32_t bus, uint32_t dev, uint32_t fun, struct dmar_entry * p_context_e)
{
	uint64_t fpd, tt, slptptr, aw, did, P;

	P = dmar_get_bitslice(p_context_e->lo_64, CTX_ENTRY_LOWER_P_MASK, CTX_ENTRY_LOWER_P_POS);
	tt = dmar_get_bitslice(p_context_e->lo_64, CTX_ENTRY_LOWER_TT_MASK, CTX_ENTRY_LOWER_TT_POS);
	fpd = dmar_get_bitslice(p_context_e->lo_64, CTX_ENTRY_LOWER_FPD_MASK, CTX_ENTRY_LOWER_FPD_POS);
	slptptr = p_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
	aw = dmar_get_bitslice(p_context_e->hi_64, CTX_ENTRY_UPPER_AW_MASK, CTX_ENTRY_UPPER_AW_POS);
	did = dmar_get_bitslice(p_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);

	pr_err("%s: BDF [%lx:%2lx:%lx]: DID:%3d, TT:%llx, AW:%llx, FPD:%llx, P:%d, SL-PTPTR: 0x%llx.", str, bus, dev, fun, did, tt, aw, fpd, P, slptptr);
	return 0;
}

int dump_root_table(uint64_t rta, int dmar_index)
{
	int i, j, ctx_cnt = 0;
	struct dmar_entry *root_entry, *ctp;
	static uint32_t bitmap;

	pr_err("%s, DMAR%d: RTA: 0x%llx", __func__, dmar_index, rta);
	root_entry = (struct dmar_entry *)(rta & PAGE_MASK);
	for (i = 0; i < 3; i++) { //bus
		if (root_entry[i].lo_64 & 1) {
			dump_root_entry("dmup-root-e", i, &root_entry[i]);
			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & PAGE_MASK);
			for (j = 0; j < 256; j++) {//df
				if (ctp[j].lo_64 & 1) {
					ctx_cnt++;
					if (dump_context_entry("dump-CTX-e", i, (j >> 3) & 0x1f, j & 0x7, &ctp[j]))
						return -1;
				}
			}
		}
	}

	bitmap |= (1 << dmar_index);
	pr_err("DMAR%d: Dump %d Context Entries.\n", dmar_index, ctx_cnt);
	return 0;
}

#define MAX_DID 64
#define MAX_IOMMU 8
static bool sanitty_chk_print;
static uint64_t  nr_max_print;
struct sanity_chk_domain {
	uint64_t hit_cnt;
	uint64_t gpa2hpa_err;
	uint64_t miss_cnt;
	uint64_t addr_err;
	uint64_t size_err;
	uint64_t permit_err;
	uint64_t nr_err;
	uint64_t nr_verified;
	uint64_t nr_rmrr;
	uint64_t rmrr_bottom;
	uint64_t rmrr_top;

	uint64_t nr_hpa_mapped_rmrr;
	uint64_t rmrr_hpa_mapped_bottom;
	uint64_t rmrr_hpa_mapped_top;
};

struct sanity_chk_viommu {
	struct sanity_chk_domain dom[MAX_DID];
};

static bool iommu_rsvd_region(__unused struct acrn_viommu *viommu, __unused uint32_t did, uint64_t iova, uint64_t gpa)
{
	return (iova == gpa);
}

struct sanity_chk_viommu sanity_chk_iommu_unit[MAX_IOMMU];

void reset_sanity_data(void)
{
	int i, j;
	struct sanity_chk_domain *dom;

	memset((void *)&sanity_chk_iommu_unit[0], 0, MAX_IOMMU * sizeof(struct sanity_chk_viommu));
	for (i = 0; i < MAX_IOMMU; i++) {
		for (j = 0; j < MAX_DID; j++) {
			dom = &(sanity_chk_iommu_unit[i].dom[j]);
			dom->rmrr_bottom = (uint64_t)-1;
			dom->rmrr_top = 0UL;

			dom->rmrr_hpa_mapped_bottom = (uint64_t)-1;
			dom->rmrr_hpa_mapped_top = 0UL;
		}
	}
}

uint64_t validate_guest_mapping_in_shadow(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t gpa, uint64_t size, uint64_t permit)
{
	const uint64_t *pte;
	uint64_t hpa_g, hpa_s, shadow_pml4, pte_size;
	int index = viommu->drhd_rt->index;
	struct acrn_vm *vm = viommu->vm;
	struct sanity_chk_domain *dom = &(sanity_chk_iommu_unit[index].dom[did]);

	dom->nr_verified++;

	if ((size != PTE_SIZE) && (size != PDE_SIZE) && (size != PDPTE_SIZE)) {
		pr_err("Invalid size:0x%x from guest mapping: DMAR%d, did:%d, iova:%llx", size, index, did, iova);
		return 0;
	}

	hpa_g = gpa2hpa(vm, gpa);
	if (iova == gpa) { /*RMRR Case: IOVA equals to GPA*/
		dom->nr_rmrr++;
		if (iova < dom->rmrr_bottom)
			dom->rmrr_bottom = iova;
		if (iova + size > dom->rmrr_top)
			dom->rmrr_top = iova + size;

		if (hpa_g != INVALID_HPA) {
			if (hpa_g == gpa) {
				dom->nr_hpa_mapped_rmrr++;
				if (iova < dom->rmrr_hpa_mapped_bottom)
					dom->rmrr_hpa_mapped_bottom = iova;
				if (iova + size > dom->rmrr_hpa_mapped_top)
					dom->rmrr_hpa_mapped_top = iova + size;
			} else {
				pr_err("Misc mapping: DMAR%d, did:%d, iova:%llx, gpa:%llx, hpa:%llx", index, did, iova, gpa, hpa_g);
			}
		}


	} else if (hpa_g == INVALID_HPA) {
		dom->gpa2hpa_err++;
		if ((sanitty_chk_print) && (dom->gpa2hpa_err <= nr_max_print)) {
			pr_err("GPA2HPA Err: DMAR%d, did:%d, iova:%llx, gpa:%llx, guest pte_size:%lld", index, did, iova, gpa, size);
		}
		goto err;
	}

	shadow_pml4 = get_shadow_pml4(viommu, did);
	pte = pgtable_lookup_entry((uint64_t *)shadow_pml4, iova, &pte_size, &(viommu->shadow_pgtable));
	if (pte == NULL) {
		dom->miss_cnt++;
		if ((sanitty_chk_print) && (dom->miss_cnt <= nr_max_print)) {
			pr_err("Miss shadow entry for guest mapping: DMAR%d, did:%d, iova:%llx, gpa:%llx, size:%llx", index, did, iova, gpa, size);
		}
		goto err;
	}

	hpa_s = (((*pte & (~EPT_PFN_HIGH_MASK)) & (~(pte_size - 1UL))) | (iova & (pte_size - 1UL)));
	if ((hpa_s != hpa_g) && !iommu_rsvd_region(viommu, did, iova, gpa)) {
		dom->addr_err++;
		if ((sanitty_chk_print) && (dom->addr_err <= nr_max_print)) {
			pr_err("Addr Mismatch: DMAR%d, did:%d, iova:%llx, gpa:%llx, guest pte_sz:%llx, shadow pte_sz:%lld hpa_guest:%llx, hpa_shadow:%llx",
				index, did, iova, gpa, size, pte_size, hpa_g, hpa_s);
		}
		goto err;
	}

	if (pte_size != size) {
		dom->size_err++;
		if ((sanitty_chk_print) && (dom->size_err <= nr_max_print)) {
			pr_err("Size Mismatch: DMAR%d, did:%d, iova:%llx, gpa:%llx, guest size:%llx, shadow size:%llx", index, did, iova, gpa, size, pte_size);
		}
		goto err;
	}

	if ((permit & EPT_RWX) != ((*pte) & EPT_RWX)) {
		dom->permit_err++;
		if ((sanitty_chk_print) && (dom->permit_err <= nr_max_print)) {
			pr_err("Permit Mismatch: DMAR%d, did:%d, iova:%llx, gpa:%llx, guest permit:%llx, shadow permit:%llx", index, did, iova, gpa, permit, (*pte) & EPT_RWX);
		}
		goto err;
	}

	dom->hit_cnt++;
	return 0;

err:
	dom->nr_err++;
	return 0;
}

bool verify_guest_pml4_addr(struct acrn_viommu *viommu)
{
	bool status = true;
	uint16_t guest_did;
	uint64_t guest_pml4, guest_rta;
	uint16_t i, j;
	struct dmar_entry *root_entry, *ctp;
	struct dmar_entry *p_guest_context_e;

	guest_rta = get_guest_rta(viommu);
	root_entry = (struct dmar_entry *)(guest_rta & PAGE_MASK);
	for (i = 0; i < 3; i++) { //bus
		if (root_entry[i].lo_64 & 1) {
			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & (~0xfff));
			for (j = 0; j <= 255; j++) {//df
				p_guest_context_e = &ctp[j];
				if (p_guest_context_e->lo_64 & 1) {

					guest_did = dmar_get_bitslice(p_guest_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);
					guest_pml4 = p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;

					if (viommu->guest_pml4[guest_did] != guest_pml4) {
						pr_err("%s, Mismatch guest PML4 vDMAR%d DID=%d, guest PML4: 0x%llx, cached pml4:0x%llx",
							__func__, viommu->drhd_rt->index, guest_did, guest_pml4, viommu->guest_pml4[guest_did]);
						status = false;
						goto exit;
					}
				}
			}
		}
	}

exit:
	return status;
}

static void dump_sanity_chk_info(struct acrn_viommu *viommu, uint16_t did, struct sanity_chk_domain *dom, bool guest_table)
{
	int dmar_index = viommu->drhd_rt->index;

	if (dom->nr_verified == 0UL)
		return;

	if (guest_table) {
		pr_err("DMAR%d, DID: %-2d Entry Count: %-8lld PASS: %-8lld FAIL: %-8lld Shadow Map: %-8lld Shadow Unamp: %-8lld",
			dmar_index, did, dom->nr_verified, dom->hit_cnt, dom->nr_err, viommu->map_cnt[did], viommu->unmap_cnt[did]);

		if (dom->nr_rmrr > 0UL) {
			pr_err("=>RMRR Info: DMAR%d, DID: %-2d, RMRR Num: %lld, Bottom Addr:0x%llx, Top Addr: 0x%llx, Size: %lld(KB)",
				dmar_index, did, dom->nr_rmrr, dom->rmrr_bottom, dom->rmrr_top, (dom->rmrr_top - dom->rmrr_bottom) >> 10);
		}

		if (dom->nr_hpa_mapped_rmrr > 0UL) {
			pr_err("=>RMRR(HPA_MAPPED) Info: DMAR%d, DID: %-2d, Num: %lld, Bottom Addr:0x%llx, Top Addr: 0x%llx, Size: %lld(KB)",
				dmar_index, did, dom->nr_hpa_mapped_rmrr, dom->rmrr_hpa_mapped_bottom,
				dom->rmrr_hpa_mapped_top, (dom->rmrr_hpa_mapped_top - dom->rmrr_hpa_mapped_bottom) >> 10);
		}

		if (dom->nr_err > 0) {
			pr_err("=>ERROR Info: DMAR%d, DID: %-2d, Err Num: %lld, gpa2hpa Err:%lld, Shadow Missing: %lld, Addr Err: %lld, Size Err: %lld, Permit Err: %lld",
				dmar_index, did, dom->nr_err, dom->gpa2hpa_err, dom->miss_cnt, dom->addr_err, dom->size_err, dom->permit_err);
		}

	} else {
		pr_err("DMAR%d, DID: %-2d Entry Count: %-8lld Shadow Map: %-8lld Shadow Unamp: %-8lld",
			dmar_index, did, dom->nr_verified, viommu->map_cnt[did], viommu->unmap_cnt[did]);

		if (dom->nr_rmrr > 0UL) {
			pr_err("=>RMRR Info: DMAR%d, DID: %-2d, RMRR Num: %lld, Bottom Addr:0x%llx, Top Addr: 0x%llx, Size: %lld(KB)",
				dmar_index, did, dom->nr_rmrr, dom->rmrr_bottom, dom->rmrr_top, (dom->rmrr_top - dom->rmrr_bottom) >> 10);
		}
	}
}

static void compare_guest_and_shadow_pgtable(int dmar_index, uint16_t did, bool print_err, uint64_t max_print)
{
	uint32_t i, index;
	struct acrn_viommu *viommu;
	struct sanity_chk_domain *dom;

	reset_sanity_data();
	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		viommu = &viommu_units[i];
		index = viommu->drhd_rt->index;
		if (!verify_guest_pml4_addr(viommu)) {
			return;
		}

		if (dmar_index == (int)index) {
			break;
		}
	}

	if (i == plat_dmar_info.drhd_count) {
		pr_err("%s Invalid dmar_index:%d", __func__, dmar_index);
		return;
	}

	if ((viommu->shadow_pml4[did] == 0UL) || (viommu->guest_pml4[did] == 0UL)) {
		pr_err("%s Invalid DMAR%d did:%d PML4: shadow_pml4:%llx, guest_pml4:%llx%d",
			__func__, dmar_index, did, viommu->shadow_pml4[did], viommu->guest_pml4[did]);
		return;
	}

	sanitty_chk_print = print_err;
	nr_max_print = max_print;

	walk_guest_pgtable(viommu, did, validate_guest_mapping_in_shadow);
	dom = &(sanity_chk_iommu_unit[index].dom[did]);
	dump_sanity_chk_info(viommu, did, dom, true);
}

static void compare_guest_and_shadow_pgtable_all(bool print_err, uint64_t max_print)
{
	uint32_t i, index;
	uint16_t did;
	struct acrn_viommu *viommu;
	struct sanity_chk_domain *dom;

	reset_sanity_data();

	sanitty_chk_print = print_err;
	nr_max_print = max_print;

	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		viommu = &viommu_units[i];
		index = viommu->drhd_rt->index;
		if (!verify_guest_pml4_addr(viommu)) {
			return;
		}

		for (did = 0; did < MAX_GUEST_IOMMU_DID; did++) {
			if ((viommu->shadow_pml4[did] != 0UL) && (viommu->guest_pml4[did] != 0UL)) {
				walk_guest_pgtable(viommu, did, validate_guest_mapping_in_shadow);
			}
		}
	}

	for (i = 0; i < plat_dmar_info.drhd_count; i++) {
		viommu = &viommu_units[i];
		index = viommu->drhd_rt->index;
		for (did = 0; did < MAX_GUEST_IOMMU_DID; did++) {
			if (viommu->guest_pml4[did] != 0UL) {
				dom = &(sanity_chk_iommu_unit[index].dom[did]);
				dump_sanity_chk_info(viommu, did, dom, true);
			}
		}
	}
}

uint64_t validate_shadow_mappings(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t gpa, uint64_t size, __unused uint64_t permit)
{
	int index = viommu->drhd_rt->index;
	struct sanity_chk_domain *dom = &(sanity_chk_iommu_unit[index].dom[did]);

	dom->nr_verified++;

	if ((size != PTE_SIZE) && (size != PDE_SIZE) && (size != PDPTE_SIZE)) {
		pr_err("Invalid size:0x%x from guest mapping: DMAR%d, did:%d, iova:%llx", size, index, did, iova);
		return 0;
	}

	if (iova == gpa) { /*RMRR Case: IOVA equals to GPA*/
		dom->nr_rmrr++;
		if (iova < dom->rmrr_bottom)
			dom->rmrr_bottom = iova;
		if (iova + size > dom->rmrr_top)
			dom->rmrr_top = iova + size;
	}

	return 0;
}

void walk_shadow_pgtable(struct acrn_viommu *viommu, uint16_t did, pgtable_mapping_handler leaf_handler)
{
	uint64_t *pml4e, *pdpte, *pde, *pte;
	uint64_t i, j, k, m;
	uint64_t iova, gpa;
	const struct pgtable *table = &viommu->shadow_pgtable;
	uint64_t pml4 = get_shadow_pml4(viommu, did);
	uint64_t nr_1g = 0, nr_2m = 0, nr_4k = 0;

	for (i = 0UL; i < PTRS_PER_PML4E; i++) {
		pml4e = pml4e_offset((uint64_t *)pml4, i << PML4E_SHIFT);
		if (!pgentry_present(table, (*pml4e))) {
			continue;
		}
		for (j = 0UL; j < PTRS_PER_PDPTE; j++) {
			pdpte = pdpte_offset(pml4e, j << PDPTE_SHIFT);
			if (!pgentry_present(table, (*pdpte))) {
				continue;
			}
			if (pdpte_large(*pdpte) != 0UL) {
				iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT);
				gpa = (*pdpte & (~EPT_PFN_HIGH_MASK)) & (~(PDPTE_SIZE - 1UL));
				leaf_handler(viommu, did, iova, gpa, PDPTE_SIZE, ((*pdpte) & EPT_RWX));
				nr_1g++;
				continue;
			}
			for (k = 0UL; k < PTRS_PER_PDE; k++) {
				pde = pde_offset(pdpte, k << PDE_SHIFT);
				if (!pgentry_present(table, (*pde))) {
					continue;
				}
				if (pde_large(*pde) != 0UL) {
					iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT) | (k << PDE_SHIFT);
					gpa = (*pde & (~EPT_PFN_HIGH_MASK)) & (~(PDE_SIZE - 1UL));
					nr_2m++;
					leaf_handler(viommu, did, iova, gpa, PDE_SIZE, ((*pde) & EPT_RWX));
					continue;
				}
				for (m = 0UL; m < PTRS_PER_PTE; m++) {
					pte = pte_offset(pde, m << PTE_SHIFT);
					if (pgentry_present(table, (*pte))) {
						iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT) | (k << PDE_SHIFT) | (m << PTE_SHIFT);
						gpa = (*pte & (~EPT_PFN_HIGH_MASK)) & (~(PTE_SIZE - 1UL));
						nr_4k++;
						leaf_handler(viommu, did, iova, gpa, PTE_SIZE, ((*pte) & EPT_RWX));
					}
				}
			}
		}
	}
	pr_err("%s: DMAR%d done, did:%d, nr_1g:%lld, nr_2m:%lld, nr_4k:%lld", __func__, viommu->drhd_rt->index, did, nr_1g, nr_2m, nr_4k);
}


static void check_shadow_pgtable_all(bool print_err, uint64_t max_print)
{
	uint32_t i, index;
	uint16_t did;
	struct acrn_viommu *viommu;
	struct sanity_chk_domain *dom;

	reset_sanity_data();

	sanitty_chk_print = print_err;
	nr_max_print = max_print;

	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		viommu = &viommu_units[i];
		index = viommu->drhd_rt->index;
		if (!verify_guest_pml4_addr(viommu)) {
			return;
		}

		for (did = 0; did < MAX_GUEST_IOMMU_DID; did++) {
			if ((viommu->shadow_pml4[did] != 0UL) && (viommu->guest_pml4[did] != 0UL)) {
				walk_shadow_pgtable(viommu, did, validate_shadow_mappings);
			}
		}
	}

	for (i = 0; i < plat_dmar_info.drhd_count; i++) {
		viommu = &viommu_units[i];
		index = viommu->drhd_rt->index;
		for (did = 0; did < MAX_GUEST_IOMMU_DID; did++) {
			if (viommu->guest_pml4[did] != 0UL) {
				dom = &(sanity_chk_iommu_unit[index].dom[did]);
				dump_sanity_chk_info(viommu, did, dom, false);
			}
		}
	}
}


#if CHECK_TIME
#define MAX_TIME_RCD 5000000
static char *tbl_name[RECORD_MAX_TBL_NUM] = {
	"RECORD_SHADOW_MAP",
	"RECORD_SHADOW_UNMAP",
	"RECORD_IOTLB"
};
static uint32_t record_tbl[RECORD_MAX_TBL_NUM][MAX_TIME_RCD];
static uint64_t record_index[RECORD_MAX_TBL_NUM];
static bool record_index_overflow[RECORD_MAX_TBL_NUM];
void insert_time(int tbl_i, uint64_t us)
{
	if (tbl_i >= RECORD_MAX_TBL_NUM) {
		pr_err("tbl index %d err, us: %lld", tbl_i, us);
	}

	record_tbl[tbl_i][record_index[tbl_i]++] = (uint16_t)us;
	if (record_index[tbl_i] >= MAX_TIME_RCD) {
		record_index[tbl_i] = 0UL;
		record_index_overflow[tbl_i] = true;
		pr_err("Warning: %s Table index overflow.", tbl_name[tbl_i]);
	}
}

void time_sum(void)
{
	uint64_t i, j, sum[RECORD_MAX_TBL_NUM], av[RECORD_MAX_TBL_NUM], max[RECORD_MAX_TBL_NUM], min[RECORD_MAX_TBL_NUM];

	for (i = 0; i < RECORD_MAX_TBL_NUM; i++) {
		min[i] = (uint64_t)-1;
		max[i] = 0UL;
		sum[i] = 0UL;
		av[i] = 0UL;
	}

	for (i = 0; i < RECORD_MAX_TBL_NUM; i++) {
		for (j = 0; j < record_index[i]; j++) {
			sum[i] += record_tbl[i][j];

			 if (record_tbl[i][j] > max[i])
				max[i] = record_tbl[i][j];

			 if (record_tbl[i][j] < min[i])
				min[i] = record_tbl[i][j];

		}
		if (record_index[i] > 0UL)
			av[i] = sum[i]/record_index[i];
	}

	if (record_index_overflow[RECORD_SHADOW_MAP] || record_index_overflow[RECORD_SHADOW_UNMAP] ||record_index_overflow[RECORD_IOTLB]) {
		pr_err("Index May Overflow in %s: %s, %s: %s, %s: %s",
			tbl_name[RECORD_SHADOW_MAP], record_index_overflow[RECORD_SHADOW_MAP] ? "TRUE" : "FALSE",
			tbl_name[RECORD_SHADOW_UNMAP], record_index_overflow[RECORD_SHADOW_UNMAP] ? "TRUE" : "FALSE",
			tbl_name[RECORD_IOTLB], record_index_overflow[RECORD_IOTLB] ? "TRUE" : "FALSE");

	} else if ((record_index[RECORD_SHADOW_MAP] > 0) && (record_index[RECORD_SHADOW_UNMAP] > 0) && (record_index[RECORD_IOTLB] > 0)) {
		pr_err("Map Percent: %lld%%, Umap Percent: %lld%%.", (sum[RECORD_SHADOW_MAP] * 100)/sum[RECORD_IOTLB], (sum[RECORD_SHADOW_UNMAP] * 100)/sum[RECORD_IOTLB]);
	} else {
		pr_err("Info is NOT enough to get time consumed by Map & Unmap.");
	}

	for (i = 0; i < RECORD_MAX_TBL_NUM; i++) {
		if (record_index[i] > 0UL)
			pr_err("%s: Record Index: %lld, av: %lld (us), max: %lld, min: %lld", tbl_name[i], record_index[i], av[i], max[i], min[i]);
	}
}

#endif /*#if CHECK_TIME*/
#define GUEST_MAPPING_LOOKUP		0 /* full param list.*/
#define SHADOW_MAPPING_LOOKUP		1 /* full param list*/
#define SHOW_SHADOW_TBL_ADDR		2 /* op only */
#define DUMP_GUEST_CONTEXT_TBL		3 /* op only */
#define DUMP_HOST_CONTEXT_TBL		4 /* op only */
#define MAP_UNMAP_CNT			5 /* op only */
#define READ_HOST_IOMMU_REG		7 /* op, dmar_index, did = 0, offset = addr, size = nr_pages*/
#define COMPARE_GUEST_AND_SHADOW_MAPPING	 	8 /* op  dmar_index, did, print_err = (addr != 0UL)*/
#define COMPARE_GUEST_AND_SHADOW_MAPPING_ALL	9 /* op  print_err = (dmar_index != 0) */
#define CHECK_SHADOW_MAPPING_ALL	10 /* op  print_err = (dmar_index != 0) */
#define SHOW_TIME_COST			11 /* op */
void viommu_debug(uint64_t op, uint64_t dmar_index, uint64_t did, uint64_t addr, uint64_t nr_pages)
{
	uint32_t i, j, loop = 0;
	struct acrn_viommu *vtd;
	uint64_t pml4, size = 0;
	const uint64_t *pte;
	uint64_t addr_end = addr + (nr_pages << 12);
	struct dmar_drhd_rt *iommu = NULL;
	uint64_t value;
	uint32_t offset;

//	pr_err("%s INPUT: op:%d, dmar_index:%d, did:%d, addr:0x%llx, nr_pages:%d", __func__, op, dmar_index, did, addr, nr_pages);
	if (op == READ_HOST_IOMMU_REG) {
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &viommu_units[i];
			if (dmar_index == vtd->drhd_rt->index) {
				iommu = vtd->drhd_rt;
				break;
			}
		}

		if (iommu == NULL) {
			pr_err("Target DMAR%d not found.", dmar_index);
			return;
		}

		offset = addr;
		size =(uint32_t)nr_pages;
		if (size == 4UL) {
			value = iommu_read32(iommu, offset);
		} else if (size == 8UL) {
			value = iommu_read64(iommu, offset);
		} else {
			pr_err("Invalid MMIO READ Size:%d.", size);
			return;
		}

		pr_err("DMAR%d, Register Read: offset: 0x%x, size: %d, value: 0x%llx.", dmar_index, offset, size, value);
		return ;
	}

	if ((op == GUEST_MAPPING_LOOKUP) || (op == SHADOW_MAPPING_LOOKUP)) {
		if (did >= MAX_GUEST_IOMMU_DID) {
			pr_err("%s, invalid did:%d", __func__, did);
			return;
		}
		vtd = NULL;
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &viommu_units[i];
			if (dmar_index == vtd->drhd_rt->index)
				break;
		}
		if (!vtd) {
			pr_err("%s, pml4 is null, did:%d", __func__, did);
			return;
		}

		if (op == GUEST_MAPPING_LOOKUP) {
			pml4 = vtd->guest_pml4[did];
		} else if (op == SHADOW_MAPPING_LOOKUP){
			pml4 = vtd->shadow_pml4[did];
		}
		if (pml4 == 0UL) {
			pr_err("%s, pml4 is null, did:%d", __func__, did);
			return;
		}

		while (addr < addr_end) {
			pr_err("pgcheck, addr:0x%llx, addr end:0x%llx.",addr, addr_end);
			pte = pgtable_lookup_entry((uint64_t *)pml4, addr, &size,  &guest_pgtable);
			if (pte == NULL) {
				pr_err("DMAR%d, %sPageTable, mapping of %llx is NOT present, loop:%d",
					dmar_index, op == GUEST_MAPPING_LOOKUP ? "Guest ": "Shadow ", addr, loop);
				addr += 4096;
				break;
			} else {
				pr_err("DMAR%d, %sPageTable, mapping of %llx is %llx(Present), size:%x, loop:%d.",
					dmar_index, op == GUEST_MAPPING_LOOKUP ? "Guest ": "Shadow ", addr, *pte, size, loop);
				addr += size;
			}
			loop++;
		}
		return;
	}

	if (op == SHOW_SHADOW_TBL_ADDR) {
		pr_err("Dump Shadow and Geust PML4:");
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &viommu_units[i];
			for (j = 0; j < MAX_GUEST_IOMMU_DID; j++) {
				if ((vtd->guest_pml4[j] == 0UL) && (vtd->shadow_pml4[j] == 0UL)) {
					continue;
				} else if ((vtd->guest_pml4[j] != 0UL) && (vtd->shadow_pml4[j] != 0UL)) {
					pr_err("DMAR%d, Guest PML4 of DID[%03d]:0x%llx, Shadow PML4:0x%llx.", vtd->drhd_rt->index, j, vtd->guest_pml4[j], vtd->shadow_pml4[j]);
				} else {
					pr_err("Error: DMAR%d, Guest PML4 of DID[%03d]:0x%llx, Shadow PML4:0x%llx.", vtd->drhd_rt->index, j, vtd->guest_pml4[j], vtd->shadow_pml4[j]);
					break;
				}
			}
		}
		return;
	}

	if (op == DUMP_GUEST_CONTEXT_TBL) {
		pr_err("Dump Guest Context:");
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &viommu_units[i];
			dump_root_table(get_guest_rta(vtd), vtd->drhd_rt->index);
		}
		return;
	}

	if (op == DUMP_HOST_CONTEXT_TBL) {
		pr_err("Dump Host Context:");
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &viommu_units[i];
			dump_root_table(iommu_read64(vtd->drhd_rt, DMAR_RTADDR_REG), vtd->drhd_rt->index);
		}
		return;
	}

	if (op == MAP_UNMAP_CNT) {
		pr_err("Dump Shadow Map and Unmap Counter:");
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &viommu_units[i];
			for (j = 0; j < MAX_GUEST_IOMMU_DID; j++) {
				if ((vtd->map_cnt[j] != 0UL) || (vtd->unmap_cnt[i] != 0UL))
					pr_err("DMAR%d, DID:%3d, Map count:%8lld, Unmap count:%8lld.",
						vtd->drhd_rt->index, j, vtd->map_cnt[j], vtd->unmap_cnt[j]);
			}

		}
		return;
	}

	if (op == COMPARE_GUEST_AND_SHADOW_MAPPING) {
		compare_guest_and_shadow_pgtable(dmar_index, did, addr != 0UL, nr_pages /*max print message*/);
		return;
	}

	if (op == COMPARE_GUEST_AND_SHADOW_MAPPING_ALL) {
		compare_guest_and_shadow_pgtable_all(dmar_index != 0, did /*max_print message*/);
		return;
	}

	if (op == CHECK_SHADOW_MAPPING_ALL) {
		check_shadow_pgtable_all(dmar_index != 0, did /*max_print message*/);
		return;
	}

	if (op == SHOW_TIME_COST) {
#if CHECK_TIME
		time_sum();
#else
		pr_err("Enable CHECK_TIME first!");
#endif
		return;
	}

	pr_err("%s, Unhandled op:%d", __func__, op);
	return;
}
#else
void viommu_debug(uint64_t op, uint64_t dmar_index, uint64_t did, uint64_t addr, uint64_t nr_pages)
{
	pr_err("%s, vIOMMU debug function is NOT enabled.", __func__);
}
#endif //VIOMMU_DEBUG
