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

#define SHADOW_DBG 1

#if SHADOW_DBG
extern int dbg_mapping;

#define SYNC_TBL	0
#define G_TBL_LOOKUP	1
#define H_TBL_LOOKUP	2
#define H_TBL_MAP	3
#define H_TBL_UNMAP	4
#define H_IOTLB_PSI	5
#define MAX_TBL		6

#define MAX_TIME_RCD 10000000

static char *tbl_name[MAX_TBL] = {
	"SYNC_TBL",
	"G_TBL_LOOKUP",
	"H_TBL_LOOKUP",
	"H_TBL_MAP",
	"H_TBL_UNMAP",
	"H_IOTBL_PSI"
};
static void insert_time(int tbl, uint64_t us);
#endif

typedef uint64_t (*pgtable_mapping_handler)(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t gpa, uint64_t size, uint64_t permit);

/* TODO: every DMAR in every guest should have one vIOMMU */
static struct acrn_viommu vdmar_drhd_units[MAX_DRHDS] = {0};

#define GET_BITS  dmar_get_bitslice
#define SET_BITS  dmar_set_bitslice

static uint32_t viommu_read32(const struct acrn_viommu *viommu, uint32_t offset)
{
	return  *((uint32_t *)(hpa2hva(viommu->regs + offset)));
}

static uint64_t viommu_read64(const struct acrn_viommu *viommu, uint32_t offset)
{
	return *((uint64_t *)(hpa2hva(viommu->regs + offset)));
}

static void viommu_write32(const struct acrn_viommu *viommu, uint32_t offset, uint32_t value)
{
	*((uint32_t *)(hpa2hva(viommu->regs + offset))) = value;
}

static void viommu_write64(const struct acrn_viommu *viommu, uint32_t offset, uint64_t value)
{
	*((uint64_t *)(hpa2hva(viommu->regs + offset))) = value;
}

int dump_root_table(uint64_t rta, int dmar_index);
#define VIOMMU_SHADOW_PGTABLE_SIZE (3 << 20) //todo

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

struct pgtable pgtable_ops = {
	.default_access_right = 0UL, /* uint64_t default_access_right;*/
	.pgentry_present_mask = EPT_RWX,
	.pool = NULL, /* struct page_pool *pool; */
	.large_page_support = NULL, /* bool (*large_page_support)(enum _page_table_level level, uint64_t prot); */
	.clflush_pagewalk = NULL, /* void (*clflush_pagewalk)(const void *p); */
	.tweak_exe_right = NULL, /* void (*tweak_exe_right)(uint64_t *entry); */
	.recover_exe_right = NULL, /* void (*recover_exe_right)(uint64_t *entry); */
};

#define VIOMMU_MAX_SHADOW_NUM (MAX_DRHDS)
#define VIOMMU_IOVA_SPACE_SIZE (MEM_4G)
static struct page *viommu_shadow_pages[VIOMMU_MAX_SHADOW_NUM];
static uint64_t *viommu_shadow_page_bitmap[VIOMMU_MAX_SHADOW_NUM];
static struct page viommu_shadow_dummy_pages[VIOMMU_MAX_SHADOW_NUM];

/* ept: extended page pool*/
static struct page_pool viommu_shadow_page_pool[VIOMMU_MAX_SHADOW_NUM];

#define VIOMMU_SHADOW_PML4_PAGE_NUM	PML4_PAGE_NUM(MAX_PHY_ADDRESS_SPACE)
#define VIOMMU_SHADOW_PDPT_PAGE_NUM	PDPT_PAGE_NUM(MAX_PHY_ADDRESS_SPACE)

static bool is_leaf_ept_entry(uint64_t ept_entry, enum _page_table_level pt_level)
{
	return (((ept_entry & PAGE_PSE) != 0U) || (pt_level == IA32E_PT));
}

static inline uint64_t get_guest_pml4(struct acrn_viommu *viommu, uint32_t did)
{
	return viommu->guest_pml4[did];
}

static inline uint64_t get_shadow_pml4(struct acrn_viommu *viommu, uint32_t did)
{
	return viommu->shadow_pml4[did];
}

static uint64_t viommu_get_shadow_page_num(void)
{
	uint64_t ept_pd_page_num = PD_PAGE_NUM(VIOMMU_IOVA_SPACE_SIZE);
	uint64_t ept_pt_page_num = PT_PAGE_NUM(VIOMMU_IOVA_SPACE_SIZE);

	return roundup((VIOMMU_SHADOW_PML4_PAGE_NUM + VIOMMU_SHADOW_PDPT_PAGE_NUM + ept_pd_page_num + ept_pt_page_num), 64U);
}

static void viommu_reserve_shadow_bitmap(void)
{
	uint32_t i;
	uint64_t bitmap_base;
	uint64_t bitmap_size;
	uint64_t bitmap_offset;

	bitmap_size = (viommu_get_shadow_page_num() * VIOMMU_MAX_SHADOW_NUM) / 8;
	bitmap_offset = viommu_get_shadow_page_num() / 8;

	pr_err("%s, shadow bitmap size: :%lld.\n", __func__, bitmap_size);

	bitmap_base = e820_alloc_memory(bitmap_size, ~0UL);
	set_paging_supervisor(bitmap_base, bitmap_size);

	for(i = 0; i < VIOMMU_MAX_SHADOW_NUM; i++){
		viommu_shadow_page_bitmap[i] = (uint64_t *)(void *)(bitmap_base + bitmap_offset * i);
	}
}

uint64_t viommu_get_total_shadow_4k_pages_size(void)
{
	return VIOMMU_MAX_SHADOW_NUM* (viommu_get_shadow_page_num()) * PAGE_SIZE;
}

/*
 * @brief Reserve space for EPT 4K pages from platform E820 table
 */
void viommu_reserve_buffer_for_shadow_pages(void)
{
	uint64_t page_base;
	uint16_t dmar_index;
	uint32_t offset = 0U;

	pr_err("%s, shadow pages:%lld, size of each shadow :%lld (pages)\n", __func__,
		viommu_get_shadow_page_num()* MAX_DRHDS, viommu_get_shadow_page_num);

	page_base = e820_alloc_memory(viommu_get_total_shadow_4k_pages_size(), ~0UL);
	set_paging_supervisor(page_base, viommu_get_total_shadow_4k_pages_size());
	for (dmar_index = 0U; dmar_index < VIOMMU_MAX_SHADOW_NUM; dmar_index++) {
		viommu_shadow_pages[dmar_index] = (struct page *)(void *)(page_base + offset);
		offset += viommu_get_shadow_page_num() * PAGE_SIZE;
	}

	viommu_reserve_shadow_bitmap();
}

void viommu_init_shadow_pgtable(struct acrn_viommu *vdmar, uint16_t dmar_index)
{
	struct pgtable *table;

	table = &vdmar->shadow_pgtable;

	struct page_pool * pool = &viommu_shadow_page_pool[dmar_index];
	pool->start_page = viommu_shadow_pages[dmar_index];
	pool->bitmap_size = viommu_get_shadow_page_num() / 64;
	pool->bitmap = viommu_shadow_page_bitmap[dmar_index];
	pool->dummy_page = &viommu_shadow_dummy_pages[dmar_index];

	spinlock_init(&pool->lock);
	memset((void *)pool->bitmap, 0, pool->bitmap_size * sizeof(uint64_t));
	pool->last_hint_id = 0UL;

	table->pool = pool;

	table->default_access_right = EPT_RD | EPT_WR;//EPT_RWX;
	table->pgentry_present_mask = EPT_RWX;
	table->clflush_pagewalk = shadow_clflush_pagewalk;
	table->large_page_support = shadow_large_page_support;
	table->tweak_exe_right = shadow_nop_tweak_exe_right;
	table->recover_exe_right = shadow_nop_recover_exe_right;
}

/*
 * @brief Release all pages except the PML4E page of a shadow table 
 */
void viommu_free_shadow_table(struct acrn_viommu *viommu, uint64_t *shadow_pml4)
{
	uint64_t *shadow_pml4e, *shadow_pdpte, *shadow_pde;
	uint64_t i, j, k;
	struct pgtable *table;

	table = &viommu->shadow_pgtable;
	if (shadow_pml4) {
		for (i = 0UL; i < PTRS_PER_PML4E; i++) {
			shadow_pml4e = pml4e_offset(shadow_pml4, i << PML4E_SHIFT);
			if (!pgentry_present(table, (*shadow_pml4e))) {
				continue;
			}
			for (j = 0UL; j < PTRS_PER_PDPTE; j++) {
				shadow_pdpte = pdpte_offset(shadow_pml4e, j << PDPTE_SHIFT);
				if (!pgentry_present(table, (*shadow_pdpte)) ||
				    is_leaf_ept_entry(*shadow_pdpte, IA32E_PDPT)) {
					continue;
				}
				for (k = 0UL; k < PTRS_PER_PDE; k++) {
					shadow_pde = pde_offset(shadow_pdpte, k << PDE_SHIFT);
					if (!pgentry_present(table, (*shadow_pde)) ||
					    is_leaf_ept_entry(*shadow_pde, IA32E_PD)) {
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

void walk_guest_pgtable(struct acrn_viommu *viommu, uint16_t did, pgtable_mapping_handler leaf_handler)
{
	uint64_t *pml4e, *pdpte, *pde, *pte;
	uint64_t i, j, k, m;
	uint64_t iova, gpa;
	const struct pgtable *table = &pgtable_ops;
	uint64_t guest_pml4 = get_guest_pml4(viommu, did);
	uint64_t n1g = 0, n2m = 0, n4k = 0;

	//pr_err("%s DMAR%d, did:%d, start...", __func__, viommu->drhd_rt->index, did);

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
				leaf_handler(viommu, did, iova, gpa, PDPTE_SIZE, ((*pdpte) & EPT_RWX));
				continue;
			}
			for (k = 0UL; k < PTRS_PER_PDE; k++) {
				pde = pde_offset(pdpte, k << PDE_SHIFT);
				if (!pgentry_present(table, (*pde))) {
					continue;
				}
				if (pde_large(*pde) != 0UL) {
					iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT) | (k << PDE_SHIFT);
					n2m++;
					gpa = (*pde & (~EPT_PFN_HIGH_MASK)) & (~(PDE_SIZE - 1UL));
					leaf_handler(viommu, did, iova, gpa, PDE_SIZE, ((*pde) & EPT_RWX));
					continue;
				}
				for (m = 0UL; m < PTRS_PER_PTE; m++) {
					pte = pte_offset(pde, m << PTE_SHIFT);
					if (pgentry_present(table, (*pte))) {
						iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT) | (k << PDE_SHIFT) | (m << PTE_SHIFT);
						gpa = (*pte & (~EPT_PFN_HIGH_MASK)) & (~(PTE_SIZE - 1UL));
						leaf_handler(viommu, did, iova, gpa, PTE_SIZE, ((*pte) & EPT_RWX));
					}
				}
			}
		}
	}
	//pr_err("%s done, i = %lld,  j = %lld, k = %lld, nr_1G:%d, nr_2m:%d, nr_4k:%d.", __func__, i, j, k, n1g, n2m, n4k);
}

static bool iommu_rsvd_region(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t gpa)
{
	/*Todo: RMRR region shall be parsed from native ACPI table*/
	return (iova == gpa);
}

static uint64_t shadow_sync_handler(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t gpa, uint64_t size, uint64_t permit)
{
	uint64_t hpa, pte_size, synced_size = size;
	const uint64_t *shadow_pte;
	uint64_t shadow_pml4 = viommu->shadow_pml4[did];
	uint64_t t1, t2;
//	static int max_msg;

	if (((permit != 0UL) && (size != PTE_SIZE) && (size != PDE_SIZE) && (size != PDPTE_SIZE)) ||(size == 0UL)) {
		pr_err("%s, WARNING: #DMAR%d, did: %d, iova:%llx, HIT Un-aligned size:%llx for shadow maping.", __func__, viommu->drhd_rt->index, did, iova, size);
	}

	shadow_pte = pgtable_lookup_entry((uint64_t *)shadow_pml4, iova, &pte_size, &pgtable_ops);
	if (permit != 0UL) { /* Add mapping to shadow table */
		if (shadow_pte) {
			/* corner case: mapping is already present in shadow table, remove it first */
			viommu_shadow_del_mr(viommu, shadow_pml4, iova, pte_size);
			synced_size = pte_size;
			pr_err("%s, WARNING: Mapping is already present: DMAR%d, did: %d, iova:%llx, gpa:%llx, shadow_pte:%llx, req size::%llx, shadow_pte size:%llx",
				__func__, viommu->drhd_rt->index, did, iova, gpa, *shadow_pte, size, pte_size);
		}

		hpa = gpa2hpa(viommu->vm, gpa);
		if ((hpa != INVALID_HPA) || (iommu_rsvd_region(viommu, did, iova, gpa))) { /* For RMRR region, IOVA equals to GPA */
#if SHADOW_DBG
			t1 = ticks_to_us(cpu_ticks());
#endif
			viommu_shadow_add_mr(viommu, shadow_pml4, hpa, iova, size, permit);
#if SHADOW_DBG
			t2 = ticks_to_us(cpu_ticks());
			insert_time(H_TBL_MAP, t2 - t1);
			viommu->map_cnt[did]++;
#endif
		}
	} else { /* remove mapping from from shadow */
		if ((size == PDE_SIZE) || (size == PDPTE_SIZE)) {
			pr_err("%s, Large page remove: DMAR%d, did: %d, iova:%llx, req size::%llx, shadow_pte size:%llx",
				__func__, viommu->drhd_rt->index, did, iova, size, pte_size);
		}

		if (shadow_pte) {
			#if SHADOW_DBG
			t1 = ticks_to_us(cpu_ticks());
			#endif
			viommu_shadow_del_mr(viommu, shadow_pml4, iova, pte_size);
			#if SHADOW_DBG
			t2 = ticks_to_us(cpu_ticks());
			insert_time(H_TBL_UNMAP, t2 - t1);
			viommu->unmap_cnt[did]++;
			#endif
			synced_size = pte_size;
		} else {
			synced_size = PTE_SIZE;
		}
	}
	return synced_size;
}

static void walk_guest_pgtable_range(struct acrn_viommu *viommu, uint16_t did, uint64_t addr, uint64_t size, pgtable_mapping_handler shadow_sync)
{
	uint64_t gpa, pte_size, req_size, synced_size, permit;
	const uint64_t *guest_pte;
	uint64_t iova_end = addr + size, iova = addr;
	uint64_t guest_pml4 = get_guest_pml4(viommu, did); /*todo GPA -> HVA*/
	uint64_t t_enter, t_exit;

#if SHADOW_DBG
	t_enter = ticks_to_us(cpu_ticks());
#endif

	while (iova < iova_end) {
		guest_pte = pgtable_lookup_entry((uint64_t *)guest_pml4, iova, &pte_size, &pgtable_ops);
		if (guest_pte == NULL) { /* Remve mapping from shadow table */
			req_size = iova_end - iova;
			synced_size = shadow_sync(viommu, did, iova, 0UL, req_size, 0UL);
		} else { /* Add mapping to shadow table */
			gpa = (((*guest_pte & (~EPT_PFN_HIGH_MASK)) & (~(pte_size - 1UL))) | (iova & (pte_size - 1UL)));
			permit = *guest_pte & EPT_RWX;
			if (permit == 0UL) {
				pr_err("%s, DMAR%d, did:%d, Invalid Guest PTE:0x%llx.", __func__, viommu->drhd_rt->index, did, *guest_pte);
			}
			req_size = (iova + pte_size <= iova_end) ? pte_size : iova_end - iova;
			#if 0
			if (req_size > PTE_SIZE) {
				pr_err("%s, loop:%d, DMAR%d, did:%d, REQ addr:%llx, REQ size:%llx, iova:%llx, iova_end:%llx, sync_size:%llx",
					__func__, loop, viommu->drhd_rt->index, did, addr, size, iova, iova_end, req_size);
			}
			#endif
			synced_size = shadow_sync(viommu, did, iova, gpa, req_size, permit);
		}
		iova += synced_size;
	}

#if SHADOW_DBG
	t_exit = ticks_to_us(cpu_ticks());
	insert_time(H_IOTLB_PSI, t_exit - t_enter);
#endif
}


void viommu_shadow_add_mr(struct acrn_viommu *vdmar, uint64_t *pml4_page,
	uint64_t hpa, uint64_t iova, uint64_t size, uint64_t prot)
{

//	dev_dbg(DBG_LEVEL_EPT, "%s, vm[%d] hpa: 0x%016lx gpa: 0x%016lx size: 0x%016lx prot: 0x%016x\n",
//			__func__, vm->vm_id, hpa, gpa, size, prot);

//	pr_err("%s, shadow pml4:%llx, hpa: 0x%llx iova: 0x%llx size: 0x%lx prot: 0x%llx\n",
//			__func__, pml4_page, hpa, iova, size, prot);

//	spinlock_obtain(&vm->ept_lock);

	pgtable_add_map(pml4_page, hpa, iova, size, prot, &vdmar->shadow_pgtable);

//	spinlock_release(&vm->ept_lock);

	//ept_flush_guest(vm);

}

void viommu_shadow_del_mr(struct acrn_viommu *vdmar, uint64_t *pml4_page, uint64_t iova, uint64_t size)
{
	//pr_err("%s,vm[%d] pml4:%llx, iova 0x%lx size 0x%lx\n", __func__, vdmar->vm->vm_id, pml4_page, iova, size);

	//spinlock_obtain(&vm->ept_lock);

	pgtable_modify_or_del_map(pml4_page, iova, size, 0UL, 0UL, &(vdmar->shadow_pgtable), MR_DEL);

	//spinlock_release(&vm->ept_lock);

	//ept_flush_guest(vm);
}


static void reset_host_context_table(struct acrn_viommu *viommu)
{
	uint64_t rta;
	int i, j;
	struct dmar_entry *root_entry, *ctp;

	rta = viommu->drhd_rt->root_table_addr;
//	pr_err("%s, DMAR%d: RTA: 0x%llx", __func__, dmar_index, rta);
	root_entry = (struct dmar_entry *)(rta & PAGE_MASK);
	for (i = 0; i < 3; i++) { //bus
		if (root_entry[i].lo_64 & 1) {
			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & PAGE_MASK);
			for (j = 0; j < 256; j++) {//df
				memset(&ctp[j], 0, sizeof(struct dmar_entry));
			}
		}
	}
}

static struct dmar_entry *get_shadow_context_entry(struct acrn_viommu *vtd, union pci_bdf *vbdf)
{
	uint32_t i;
	union pci_bdf pbdf;
	struct acrn_vm *vm = vtd->vm;
	struct acrn_vpci *vpci = &(vm->vpci);
	struct pci_vdev *vdev;//pci_vdevs[CONFIG_MAX_PCI_DEV_NUM];
	uint64_t native_rta = vtd->drhd_rt->root_table_addr;
	struct dmar_entry *p_rta, *p_root_e, *p_context, *p_context_e = NULL;

	p_rta = (struct dmar_entry *)vtd->drhd_rt->root_table_addr;
//	pr_err("%s enter, vBDF = [%x:%x:%x].", __func__, vbdf->bits.b, vbdf->bits.d, vbdf->bits.f);

	//use pci_find_vdev() ?
	for (i = 0; i < vpci->pci_vdev_cnt; i++) {
		vdev =&(vpci->pci_vdevs[i]);
		if (vdev->pdev->drhd_index != vtd->drhd_rt->index) {
		//	pr_err("vBDF[%d:%d:%d] belongs to DMAR%d, current DMAR index:%d", vbdf->bits.d, vbdf->bits.d, vbdf->bits.f, vdev->pdev->drhd_index, vtd->drhd_rt->index);
		}

		if ((vbdf->bits.b == vdev->bdf.bits.b) &&
			(vbdf->bits.d == vdev->bdf.bits.d) && (vbdf->bits.f == vdev->bdf.bits.f)) {

			memcpy_s(&pbdf, sizeof(union pci_bdf), &(vdev->pdev->bdf), sizeof(union pci_bdf));
			//pr_err("%s, vBDF = %d:%d:%d, pBDF = %d:%d:%d., search native context entry...",__func__,
			//	vbdf->bits.b, vbdf->bits.d, vbdf->bits.f, pbdf.bits.b, pbdf.bits.d, pbdf.bits.f);

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

#define REMAPPED_DID_OFFSET 11 /* Low 11 bits are reserved for guest DID, mask:0x7FF */
static uint32_t construct_virtual_did(uint32_t vmid, uint32_t vdid)
{
//	return ((1 << 15) | (vmid << REMAPPED_DID_OFFSET) | vdid);
	return vdid; //((1 << 15) | (vmid << REMAPPED_DID_OFFSET) | vdid);
}

static uint64_t get_guest_rta(struct acrn_viommu *viommu)
{
	return viommu_read64(viommu, DMAR_RTADDR_REG);
}

static int context_cache_inv_device(struct acrn_viommu *vtd, uint32_t did, uint32_t sid, uint32_t fm)
{
	int status = 0;
	int index = vtd->drhd_rt->index;
	uint16_t bus, devfun;
	uint16_t guest_did, remapped_did;
	uint64_t shadow_pml4, guest_pml4;
	uint16_t i, j, ctx_cnt = 0;
	struct dmar_entry *guest_root_e;
	union pci_bdf vbdf;
	struct dmar_entry *p_guest_context_e, *p_shadow_context_e;

	bus = (sid >> 8) & 0xFF;
	devfun = sid & 0xFF;
	guest_root_e = (struct dmar_entry *)(get_guest_rta(vtd) & PAGE_MASK) + bus;
	if (guest_root_e->lo_64 & 0x1) {
		p_guest_context_e = (struct dmar_entry *) (guest_root_e->lo_64 & PAGE_MASK) + devfun;
		if (p_guest_context_e->lo_64 & 0x1) {
			//dump_context_entry("Guest-CTX-e", i, (j >> 3) & 0x1f, j & 0x7, p_guest_context_e);
			//pr_err("%s, ctx_cnt:%lld.", __func__, ctx_cnt);

			guest_did = GET_BITS(p_guest_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);
			ASSERT(guest_did < MAX_GUEST_IOMMU_DID, "Guest DID overflow"); //this assert check is for did remapping logic
			#if 0
			if (guest_did != did) { //need to ignore did input value
				pr_err("%s, Err input did:%d and guest_did:%d does not match.", __func__, did, guest_did);
			}
			#endif

			//if (((cc_inv_g == VTD_INV_DESC_CC_DOMAIN) || (cc_inv_g == VTD_INV_DESC_CC_DEVICE)) && (did != guest_did))
			//	continue;

			//pr_err("%s, CC_device: did:%lld, sid:[%x:%x:%x]", __func__, VTD_INV_DESC_CC_DID(entry->lo_64), (sid >> 8) & 0xff, (sid >> 3) &0x1f, sid & 0x7);
			//if ((cc_inv_g == VTD_INV_DESC_CC_DEVICE) && ((did != guest_did) || (((sid >> 8) & 0xFF ) != i) || ((sid & 0xFF) != j)))
			//if ((cc_inv_g == VTD_INV_DESC_CC_DEVICE) && ((((sid >> 8) & 0xFF ) != i) || ((sid & 0xFF) != j)))
			//	continue;

			/*
			 * VT-d specification #9.3, Context-entries programmed with the same domain identifier
			 * must always reference same address translation(SLPTPTR field).
			 */
			if (vtd->shadow_pml4[guest_did] == 0UL) {
				/* create IOMMU shadow table for this guest IOMMU domain */
				shadow_pml4 = (uint64_t)pgtable_create_root(&vtd->shadow_pgtable);
				if (shadow_pml4 == 0UL) {
					status = -1;
					pr_err("%s, failed to create shadow table for DMAR%d, did:%d.", __func__, index, guest_did);
					goto exit;
				}
				//pr_err("%s, create shadow table for guest vDMAR%d DID=%d, shadow PML4: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, shadow_pml4);
				vtd->shadow_pml4[guest_did] = shadow_pml4;
			} else {
				shadow_pml4 = vtd->shadow_pml4[guest_did];

				//Unmap all shadow table, but keep shadow PML4 page.
				//pr_err("%s, Remove shadow mappings for DMAR%d, did:%d.", __func__, index, guest_did);
				viommu_free_shadow_table(vtd, shadow_pml4);
			}

			guest_pml4 = p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
			if (guest_pml4 == 0UL) {
				pr_err("%s, WARNING: Guest PML4 is NULL. vDMAR%d DID=%d, ctx entry: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, p_guest_context_e->lo_64);
				status = -1;
				goto exit;
			}

			if (vtd->guest_pml4[guest_did] == 0UL) {
				vtd->guest_pml4[guest_did] = (uint64_t)gpa2hva(vtd->vm, guest_pml4);//p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
				//pr_err("%s, Store guest PML4 vDMAR%d DID=%d, guest PML4: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, guest_pml4);//vtd->guest_pml4[did]);
			} else if (((uint64_t)gpa2hva(vtd->vm, guest_pml4)) != vtd->guest_pml4[guest_did]) {
				pr_err("%s, Guest is trying to update pml4 for did:%d, current guest pml4:0x%llx, cached guest pml4:0x%llx,",
					__func__, guest_did, guest_pml4, vtd->guest_pml4[guest_did]);
					vtd->guest_pml4[guest_did] = guest_pml4;
			}

			vbdf.fields.bus = bus;
			vbdf.fields.devfun = devfun;
			p_shadow_context_e = get_shadow_context_entry(vtd, &vbdf);
			if (p_shadow_context_e != NULL) {
				/* update native context entry */
				p_shadow_context_e->lo_64 = p_guest_context_e->lo_64;
				p_shadow_context_e->lo_64 &= (~CTX_ENTRY_LOWER_SLPTPTR_MASK);
				#if 1 //use shadow page table
				p_shadow_context_e->lo_64 |= (shadow_pml4 & CTX_ENTRY_LOWER_SLPTPTR_MASK);
				#else //use guest page table directly for debug purpose only.
				p_shadow_context_e->lo_64 |= (guest_pml4 & CTX_ENTRY_LOWER_SLPTPTR_MASK);
				#endif

				remapped_did = construct_virtual_did(vtd->vm->vm_id, guest_did);
				p_shadow_context_e->hi_64 = p_guest_context_e->hi_64;
				p_shadow_context_e->hi_64 &= (~CTX_ENTRY_UPPER_DID_MASK);
				p_shadow_context_e->hi_64 |= ((remapped_did << CTX_ENTRY_UPPER_DID_POS) & CTX_ENTRY_UPPER_DID_MASK);

				iommu_flush_cache(p_shadow_context_e, sizeof(struct dmar_entry));
				//dump_context_entry("Shadow  CTX", vbdf.bits.b, vbdf.bits.d, vbdf.bits.f, p_shadow_context_e);
			} else {
				pr_err("%s, fail to get native context entry, vBDF= [%x:%x:%x]", __func__, vbdf.bits.b, vbdf.bits.d, vbdf.bits.f);
				status = -1;
				goto exit;
			}

		}
	}
exit:
	return status;
}

static int context_cache_inv_global(struct acrn_viommu *vtd)
{
	int status = 0;
	uint16_t guest_did, sid, fm = 0;
	uint16_t i, j, ctx_cnt = 0;
	struct dmar_entry *root_entry, *ctp;

	uint64_t guest_rta;
	struct dmar_entry *p_guest_context_e;
	int index = vtd->drhd_rt->index;

	guest_rta = get_guest_rta(vtd);
	reset_host_context_table(vtd);
	root_entry = (struct dmar_entry *)(guest_rta & PAGE_MASK);
	for (i = 0; i < 256; i++) { //bus
		if (root_entry[i].lo_64 & 1) {
			//dump_root_entry("Guest-root-e", i, &root_entry[i]);
			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & PAGE_MASK);
			for (j = 0; j <= 255; j++) {//df
				p_guest_context_e = &ctp[j];
				if (p_guest_context_e->lo_64 & 1) {
					ctx_cnt++;

					//dump_context_entry("Guest-CTX-e", i, (j >> 3) & 0x1f, j & 0x7, p_guest_context_e);
					//pr_err("%s, ctx_cnt:%lld.", __func__, ctx_cnt);

					guest_did = GET_BITS(p_guest_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);
					ASSERT(guest_did < MAX_GUEST_IOMMU_DID, "Guest DID overflow");

					sid = (i << 8) | j;
					pr_err("%s, To invalidate one: bdf = [%x:%x:%x], guest_did:%d", __func__, i, (j >> 3) & 0x1f, j & 0x7, guest_did);
					context_cache_inv_device(vtd, guest_did, sid, fm); //ignore fm.

				}
			}
		}
	}

exit:
	//pr_err("%s, DMAR%d done %d context entries has been walked.\n", __func__, vtd->drhd_rt->index, ctx_cnt);
	return status;
}

static int iotlb_inv_domain(struct acrn_viommu *viommu, uint32_t did)
{
	uint64_t guest_pml4;
	uint64_t shadow_pml4;
	int index = viommu->drhd_rt->index;
	
	guest_pml4 = get_guest_pml4(viommu, did);
	shadow_pml4 = get_shadow_pml4(viommu, did);

	viommu_free_shadow_table(viommu, shadow_pml4);

//	pr_err("%s, DMAR%d: did:%d, guest_pml4:0x%llx, shadow_pml4:0x%llx.", __func__, index, did, guest_pml4, shadow_pml4);
	walk_guest_pgtable(viommu, did, shadow_sync_handler);
	
	return 0;	
}

static int iotlb_inv_global(struct acrn_viommu *viommu)
{
	uint32_t did, cnt = 0;
	int index = viommu->drhd_rt->index;
	uint64_t guest_pml4, shadow_pml4;
	
	for (did = 0; did < MAX_GUEST_IOMMU_DID; did++) {
		guest_pml4 = get_guest_pml4(viommu, did);
		shadow_pml4 = viommu->shadow_pml4[did];
		
		if ((guest_pml4 == 0UL) && (shadow_pml4 == 0UL)) {
				//pr_err("%s, DMAR%d, guest_pml4:0x%llx, shadow_pml4:0x%llx.", __func__, index, guest_pml4, shadow_pml4);
				continue;
		}
		
		if ((guest_pml4 == 0UL) || (shadow_pml4 == 0UL)) {
			pr_err("%s, DMAR%d, guest_pml4:0x%llx, shadow_pml4:0x%llx.", __func__, index, guest_pml4, shadow_pml4);
			return -1;
		}
		
		iotlb_inv_domain(viommu, did);
		cnt++;
	}

	//pr_err("%s done, %d domains has be invalidated.", __func__, cnt);
	return 0;
}

static int iotlb_inv_psi(struct acrn_viommu *viommu, struct dmar_entry *iotlb_inv_desc)
{
	uint64_t did, am, addr, size;
	int status = -1;
	uint64_t guest_pml4;
	uint64_t shadow_pml4;

	did = VTD_INV_DESC_IOTLB_DID(iotlb_inv_desc->lo_64);
	addr = VTD_INV_DESC_IOTLB_ADDR(iotlb_inv_desc->hi_64);
	am = VTD_INV_DESC_IOTLB_AM(iotlb_inv_desc->hi_64);
	size = ((1 << am) << 12);
	
	if (did >= MAX_GUEST_IOMMU_DID) {
		pr_err("%s, Can't support guest did:%d.\n", __func__, did);
		return -1;
	}

	guest_pml4 = get_guest_pml4(viommu, (uint32_t)did);
	shadow_pml4 = get_shadow_pml4(viommu, (uint32_t)did);
	if ((guest_pml4 != 0UL) && (shadow_pml4 != 0UL)) {
		walk_guest_pgtable_range(viommu, did, addr, size, shadow_sync_handler);
	}
	
	return status;	
}

static int process_context_cache_desc(struct acrn_viommu *viommu, struct dmar_entry *entry)
{
	int status = -1;
	uint32_t sid = 0U, did = 0U, fm = 0;
	uint64_t cc_g = entry->lo_64 & VTD_INV_DESC_CC_G;

	switch (cc_g) {
	case VTD_INV_DESC_CC_GLOBAL:
		/* On Linux, the translation table should be empty at this moment, just passthru this write */
		pr_err("%s,DMAR%d, CC_Global.", __func__, viommu->drhd_rt->index);
		status =  context_cache_inv_global(viommu); /* actually, this trap maybe not required, as all guest pgtables are empty at this point. */
		break;

	case VTD_INV_DESC_CC_DOMAIN:
		pr_err("%s,DMAR%d, CC_Domain, did:%lld.", __func__, viommu->drhd_rt->index, VTD_INV_DESC_CC_DID(entry->lo_64));
		break;

	case VTD_INV_DESC_CC_DEVICE:
		did = VTD_INV_DESC_CC_DID(entry->lo_64); /* always be 0 from linux guest. */
		sid = VTD_INV_DESC_CC_SID(entry->lo_64);
		fm = VTD_INV_DESC_CC_FM(entry->lo_64);
		//pr_err("%s,DMAR%d, CC_Device, did:%lld.", __func__, viommu->drhd_rt->index, VTD_INV_DESC_CC_DID(entry->lo_64));
		//pr_err("%s, DMAR%d, CC_Device: sid:[%x:%x:%x]", __func__,
		//	viommu->drhd_rt->index, (sid >> 8) & 0xff, (sid >> 3) &0x1f, sid & 0x7);
		status = context_cache_inv_device(viommu, did, sid, fm);
		break;

	default:
		break;
	}

	return status;
}

/* vt-d spec: 6.5.2.3 IOTLB Invalidate Descriptor */
static bool process_iotlb_desc(struct acrn_viommu *viommu, struct dmar_entry *entry)
{
	bool write_iqt = true;
	struct dmar_entry iotlb_desc;
	uint64_t addr = 0UL, am = 0UL;
	uint16_t did = 0U;
	int index = viommu->drhd_rt->index;

	switch (entry->lo_64 & VTD_INV_DESC_IOTLB_G) {
	case VTD_INV_DESC_IOTLB_GLOBAL:
		pr_err("%s, DMAR%d, IOTLB_Global.", __func__, index);
		iotlb_inv_global(viommu); /* guest page talbes are empty at this point, so it maybe skipped. */
		break;

	case VTD_INV_DESC_IOTLB_DOMAIN:
		//pr_err("%s, DMAR%d, IOTLB_Domain, did:%d.", __func__, index, (entry->lo_64 >> 16) & 0xFFFF);

		/*guest page table maybe present when guest issue domain iotlb.*/
		iotlb_inv_domain(viommu,(entry->lo_64 >> 16) & 0xFFFF);
		break;

	case VTD_INV_DESC_IOTLB_PAGE:
		if (!iommu_cap_max_amask_val(viommu->drhd_rt->cap)) {
			entry->lo_64 = (entry->lo_64 & ~VTD_INV_DESC_IOTLB_G) | VTD_INV_DESC_IOTLB_DOMAIN;
			entry->hi_64 = 0UL;
		}

		iotlb_inv_psi(viommu, entry);
		if (!(viommu->drhd_rt->cap & VTD_CAP_PSI)) { // No PSI support on host
			pr_err("%s, DMAR%d, IOTLB_PSI(Not support Natively), did:%d, iova:0x%llx, pages:%d.",
				__func__, index, (entry->lo_64 >> 16) & 0xFFFF, entry->hi_64 & (~0xfff), 1 << (entry->hi_64 & 0x3f));

			/* fallback to domain iotlb flush */
			iotlb_desc.lo_64 = DMA_IOTLB_DR | DMA_IOTLB_DW | DMA_IOTLB_DOMAIN_INVL| DMAR_INV_IOTLB_DESC;
			iotlb_desc.lo_64 |= (entry->lo_64 & IOTLB_INV_LOWER_DID_MASK);
			iotlb_desc.hi_64 = 0UL;
			dmar_issue_qi_request(viommu->drhd_rt, iotlb_desc);
			write_iqt = false; /* caller does not need to issue more IQ request for this flush.*/
		}
		break;

	default:
		break;
	}

	return write_iqt;
}

int handle_fsts_write(struct acrn_viommu *viommu, uint32_t fsts)
{
	//pr_err("%s, DMAR%d, value:%llx", __func__, viommu->drhd_rt->index, fsts);
	return 0;
}

#define VTD_FECTL_IM_MASK (1U << 31)
#define VTD_FECTL_IP_MASK (1U << 30)
int handle_fectl_write(struct acrn_viommu *viommu, uint32_t req_fectl)
{
	uint32_t fectl = viommu_read32(viommu, DMAR_FECTL_REG);
	uint32_t req_bits = req_fectl ^ fectl;

	//only IM(bit31) can be set.
	if ((req_bits & VTD_FECTL_IM_MASK) != 0U) {
		/* handle possible pending interrupt.*/
		if (req_fectl & VTD_FECTL_IM_MASK) {
			/*Set IM*/
		} else {
			/*Clear  IM*/
		}
		fectl &= (~VTD_FECTL_IM_MASK);
		fectl |= (req_fectl & VTD_FECTL_IM_MASK);
		viommu_write32(viommu, DMAR_FECTL_REG, fectl);
	}
	//pr_err("%s, DMAR%d, value:%llx", __func__, viommu->drhd_rt->index, fectl);
	return 0;
}

void generate_dmar_interrupt(struct acrn_viommu *viommu, uint32_t msi_addr_reg, uint32_t msi_data_reg)
{
	/*Read addr & data from registers*/

	/* Inject MSI interrupt to guest with message addr & data */
}

int report_fault(struct acrn_viommu *viommu, uint32_t reason)
{
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
			if (entry->lo_64 & (1 << 4)) {
				pr_err("%s, IF is set in WAIT Desc...", __func__);
			}

			if (dmar_issue_qi_complete(dmar_unit)) {
				/* set the Done Status in the wait entry */
				status_ptr = (uint32_t *)gpa2hva(viommu->vm, entry->hi_64);
				*status_ptr = (uint32_t)(entry->lo_64 >> 32U);
			}

			break;
		}

		/* Todo: Dev-TLB flush and others..?*/
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

#define UNSUPPORTED_GCMD (DMA_GCMD_CFI | DMA_GCMD_IRE | DMA_GCMD_SIRTP | DMA_GCMD_EAFL | DMA_GCMD_SFL)
int handle_gcmd(struct acrn_viommu *viommu)
{
	int status = 0;
	int index = viommu->drhd_rt->index;
	struct dmar_drhd_rt *dmar_unit = viommu->drhd_rt;
	uint32_t gcmd, req_bits, p_gsts, v_gsts = 0U;
	uint64_t rta;

	gcmd = viommu_read32(viommu, DMAR_GCMD_REG);
	v_gsts = viommu_read32(viommu, DMAR_GSTS_REG);
	req_bits = v_gsts ^ gcmd;

	//pr_err("%s, DMAR%d, gcmd:%lx, v_gsts:%lx, req_bits:%lx", __func__, index, gcmd, v_gsts, req_bits);
	ASSERT(((req_bits & UNSUPPORTED_GCMD) == 0U), "unsupported GCMD bits.");

	p_gsts = iommu_read32(dmar_unit, DMAR_GSTS_REG);
	if (req_bits & DMA_GCMD_TE) {
		if (gcmd & DMA_GCMD_TE) {
			if ((p_gsts & DMA_GSTS_TES)) {
				v_gsts |= DMA_GSTS_TES;
				//pr_err("%s, DMAR%d, TE is enabled.", __func__, index);
			} else
				pr_err("%s, DMAR%d, native TE has NOT been enabled..", __func__, index);

		} else {
			/* guest is trying to disable TE */
			v_gsts &= (~DMA_GSTS_TES);
			//pr_err("%s, DMAR%d, TE is Disabled.", __func__, index);
		}
	}

	if (req_bits & DMA_GCMD_QIE) {
		if (gcmd & DMA_GCMD_QIE) {
			if (p_gsts & DMA_GSTS_QIES) {
				v_gsts |= DMA_GSTS_QIES;
				//pr_err("%s, DMAR%d,  QIE is Enabled.", __func__, index);
			} else {
				pr_err("%s, DMAR%d, native QIE has NOT been enabled..", __func__, index);
			}
		} else {
			v_gsts &= (~DMA_GSTS_QIES);
			//pr_err("%s, DMAR%d,  QIE is Disabled.", __func__, index);
		}
	}

	if (req_bits & DMA_GCMD_SRTP) {
		if (p_gsts & DMA_GSTS_RTPS)  {
			rta = viommu_read64(viommu, DMAR_RTADDR_REG);
			if (RTA_TTM(rta) == TTM_LEGACY_MODE) { /* support legacy mode only */
				v_gsts |= DMA_GSTS_RTPS;
			} else {
				pr_err("%s, DMAR%d, Not support TTM:%lx. ", __func__, index, RTA_TTM(rta));
			}
		} else {
			pr_err("%s, DMAR%d, SRTP is not enable on host, p_gsts:%lx. ", __func__, index, p_gsts);
		}
	}

	if (req_bits & DMA_GCMD_WBF) {
		pr_err("%s, DMAR%d, WBF.", __func__, index);
		v_gsts |= (p_gsts & DMA_GSTS_WBFS); // todo double check it.
	}

	viommu_write32(viommu, DMAR_GSTS_REG, v_gsts);
	return status;
}

#define MAX_DMAR_REG_SPACE 0x1000
static uint64_t viommu_mmio_read(struct acrn_viommu *viommu, struct acrn_mmio_request *mmio)
{
	struct dmar_drhd_rt *dmar_unit= viommu->drhd_rt;
	int index = dmar_unit->index;
	uint32_t offset = mmio->address - dmar_unit->drhd->reg_base_addr;
	uint64_t value;

	if (offset + mmio->size > MAX_DMAR_REG_SPACE) {
		pr_err("%s, DMAR%d offset: 0x%x, size: %d overflow.", __func__, dmar_unit->index, offset, mmio->size);
		value = 0UL;
	}

	spinlock_obtain(&viommu->lock);

	switch (offset) {
	case DMAR_VER_REG:
		value = viommu_read64(viommu, offset); /*todo: move to default case */
		break;

	case DMAR_CAP_REG: /*todo: move to default case */
		value = viommu_read64(viommu, offset);
		break;

	case DMAR_ECAP_REG:
		value = viommu_read64(viommu, offset); /*todo: move to default case */
		break;

	case DMAR_GSTS_REG:
		value = viommu_read32(viommu, offset); /*todo: move to default case */
		//pr_err("%s,DMAR%d,  GSTS:%llx", __func__, index, value);
		break;
	case DMAR_FSTS_REG:
		value = viommu_read32(viommu, offset);
		//pr_err("%s,DMAR%d,  FSTS:%llx", __func__, index, value);
		break;

	case DMAR_FECTL_REG:
		value = viommu_read32(viommu, offset);
		//pr_err("%s,DMAR%d,  FECTL:%llx", __func__, index, value);
		break;

	case DMAR_IQT_REG:
		value = viommu_read64(viommu, DMAR_IQT_REG);
		//pr_err("%s, DMAR%d,  tail:%lx", __func__, index, value);
		break;

	case DMAR_IQH_REG:
		value = viommu_read64(viommu, DMAR_IQH_REG);
		//pr_err("%s, DMAR%d,  head:%lx", __func__, index, value);
		break;

	case DMAR_IQA_REG:
		value = viommu_read64(viommu, DMAR_IQA_REG);
		//pr_err("%s, DMAR%d,  IQA:%llx", __func__, index, value);
		break;

	default:
		if (mmio->size == 4U) {
			value = iommu_read32(dmar_unit, offset);
		} else {
			value = iommu_read64(dmar_unit, offset);
		}
		pr_err("%s, DMAR%d, Read from native: offset:0x%x, host value:0x%llx", __func__, index, offset, value);
	}

	spinlock_release(&viommu->lock);

	if ((offset != DMAR_FSTS_REG) || (value != 0U)) {
		dev_dbg(DBG_LEVEL_VIOMMU, "rd dmar%d offset %x size %x value %llx", dmar_unit->index, offset, mmio->size, value);
	}

	return value;
}

static void viommu_mmio_write(struct acrn_viommu *viommu, struct acrn_mmio_request *mmio)
{
	struct dmar_drhd_rt *dmar_unit = viommu->drhd_rt;
	uint32_t offset = mmio->address - dmar_unit->drhd->reg_base_addr;
	int index = dmar_unit->index;
	bool write_reg = false;
	uint32_t v_gsts;
	uint64_t iq_addr;

	if (offset + mmio->size > MAX_DMAR_REG_SPACE) {
		pr_err("%s, DMAR%d offset: 0x%x, size: %d overflow.", __func__, index, offset, mmio->size);
		goto exit;
	}

	//if (offset != DMAR_IQT_REG) {
	//pr_err("%s --> DMAR%d offset: 0x%x, size: %d,  value: 0x%llx", __func__, index, offset, mmio->size, mmio->value);
	//}

	spinlock_obtain(&viommu->lock);

	switch (offset) {
	case DMAR_GCMD_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		handle_gcmd(viommu);
		break;

	case DMAR_RTADDR_REG:
		viommu_write64(viommu, offset, mmio->value);
		break;

	case DMAR_FSTS_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		handle_fsts_write(viommu, mmio->value);
		break;

	case DMAR_FECTL_REG:
//		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		handle_fectl_write(viommu, mmio->value);
		break;

	case DMAR_FEDATA_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		//pr_err("%s, DMAR%d, write Fault Event DATA, data:%llx", __func__, index, mmio->value);
		break;

	case DMAR_FEADDR_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		//pr_err("%s, DMAR%d, write Fault Event Addr, addr:%llx", __func__, index, mmio->value);
		break;

	case DMAR_FEUADDR_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		//pr_err("%s, DMAR%d, write Fault Event Addr Upper, addr_upper:%llx", __func__, index, mmio->value);
		break;
#if 0
	case DMAR_PMEN_REG: /* Protected Memory Enable */
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		write_reg = false;
		break;
#endif

	case DMAR_IQT_REG:
		v_gsts = viommu_read32(viommu, DMAR_GSTS_REG);
		if (v_gsts & DMA_GSTS_QIES) {
			handle_iqt_write(viommu, mmio->value);

			/* update guest IQT & IQH */
			viommu_write64(viommu, DMAR_IQT_REG, mmio->value);
			viommu_write64(viommu, DMAR_IQH_REG, mmio->value);
		} else {
			/*Todo: Inject execepton to guest for this case? */
			//pr_err("%s, DMAR%d:  Can't write IQT as QIE is NOT enabled. guest GSTS:%lx", __func__, index, v_gsts);
		}
		write_reg = false;
		break;

	case DMAR_IQA_REG:
		iq_addr = mmio->value;
		viommu_write64(viommu, DMAR_IQA_REG, iq_addr);

		viommu->qi_queue = (uint64_t)gpa2hva(viommu->vm, iq_addr);
		viommu->qi_dw= IQ_QUEUE_DW(iq_addr);
		viommu->qi_queue_size = (PAGE_SIZE) << (iq_addr & IQ_QUEUE_QS_MASK);
		pr_err("%s, DMAR%d, IQA:%llx, DW:%llx, QS:%llx.", __func__, index, iq_addr, viommu->qi_dw, viommu->qi_queue_size);
		viommu_write64(viommu, DMAR_IQH_REG, 0UL);
		viommu_write64(viommu, DMAR_IQT_REG, 0UL);

		/* Don't write QI Addr register */
		write_reg = false;
		break;

	default:
		pr_err("%s, DMAR%d,  Unhandled Write offset:0x%x, value:0x%llx", __func__, index, offset, mmio->value);
		break;
	}

	if (write_reg) {
		pr_err("%s, DMAR%d,  WARNING: Write-thru offset:0x%x, value:0x%llx", __func__, index, offset, mmio->value);
		if (mmio->size == 4U) {
			iommu_write32(dmar_unit, offset, (uint32_t)mmio->value);
		} else {
			iommu_write64(dmar_unit, offset, mmio->value);
		}
	}

	spinlock_release(&viommu->lock);
exit:
	return;
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

static void init_readonly_registers(struct acrn_viommu *viommu)
{
	uint32_t val32;
	uint64_t val64;
	struct dmar_drhd_rt *dmar_unit = viommu->drhd_rt;
	int index = dmar_unit->index;

	/* Version */
	viommu_write32(viommu, DMAR_VER_REG, iommu_read32(dmar_unit, DMAR_VER_REG));
	pr_err("%s, DMAR%d: VT-d VER:%lx (Maj:bit7~4, Min:bit3~0)",
		__func__, index, viommu_read64(viommu, DMAR_VER_REG));

	/* Capability */
	val64 = dmar_unit->cap;
	val64 &= (~(VTD_CAP_ESIRTPS | VTD_CAP_FL5LP | VTD_CAP_PI | VTD_CAP_FL1GP | VTD_CAP_PHMR | VTD_CAP_PLMR | VTD_CAP_AFL)); /* Always clear bits. */
	val64 |= (VTD_CAP_PSI | VTD_CAP_CM); /* Always set capability bits */

	/* Set number of fault registers */
	val64 = SET_BITS(val64, VTD_CAP_NFR_MASK, VTD_CAP_NFR_POS, VTD_FCRD_REG_NR - 1U);
	viommu->frcd_index = 0U;
	viommu->frcd_offset = GET_BITS(val64,VTD_CAP_FRO_MASK, VTD_CAP_FRO_POS);

	viommu_write64(viommu, DMAR_CAP_REG, val64);
	pr_err("%s, DMAR%d: Host cap: %-16llx Guest cap: %-16llx, frcd_offset:%lx", __func__,
		index, dmar_unit->cap, viommu_read64(viommu, DMAR_CAP_REG), viommu->frcd_offset);

	/* Extend Capability */
	val64 = dmar_unit->ecap;
	/* Expose below extend capability bits only */
	val64 &= (VTD_ECAP_SC | VTD_ECAP_DT | VTD_ECAP_QI | VTD_ECAP_C);
	viommu_write64(viommu, DMAR_ECAP_REG, val64);
	pr_err("%s, DMAR%d: Host ecap: %-16llx Guest ecap: %-16llx", __func__,
		index, dmar_unit->ecap, viommu_read64(viommu, DMAR_ECAP_REG));

	/* Initialize FSTS */
	viommu_write32(viommu, DMAR_FSTS_REG, 0U);
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

		init_readonly_registers(&vdmar_drhd_units[i]);

#if SHADOW_EN
		viommu_init_shadow_pgtable(&vdmar_drhd_units[i], i);
#endif

		register_mmio_emulation_handler(vm, viommu_mmio_handler,
			dmar_unit->drhd->reg_base_addr,
			dmar_unit->drhd->reg_base_addr + PAGE_SIZE,
			(void *)&vdmar_drhd_units[i], false);

		dev_dbg(DBG_LEVEL_VIOMMU, "register MMIO %llx", dmar_unit->drhd->reg_base_addr);
	}
}

void deinit_viommu(struct acrn_vm *vm)
{
	//todo
}

#if SHADOW_DBG
/*All below are debug code */
static void dump_root_entry(char *str, int bus, struct dmar_entry * p_root_e)
{
	pr_err("%s: root entry[bus = %d], conext pointer:%llx.", str, bus, p_root_e->lo_64);
}

static int dump_context_entry(char *str, uint32_t bus, uint32_t dev, uint32_t fun, struct dmar_entry * p_context_e)
{
	uint64_t fpd, tt, slptptr, aw, did, P;

	P = GET_BITS(p_context_e->lo_64, CTX_ENTRY_LOWER_P_MASK, CTX_ENTRY_LOWER_P_POS);
	tt = GET_BITS(p_context_e->lo_64, CTX_ENTRY_LOWER_TT_MASK, CTX_ENTRY_LOWER_TT_POS);
	fpd = GET_BITS(p_context_e->lo_64, CTX_ENTRY_LOWER_FPD_MASK, CTX_ENTRY_LOWER_FPD_POS);
	slptptr = p_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
	aw = GET_BITS(p_context_e->hi_64, CTX_ENTRY_UPPER_AW_MASK, CTX_ENTRY_UPPER_AW_POS);
	did = GET_BITS(p_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);

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
static int  nr_max_print;
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
};

struct sanity_chk_viommu {
	struct sanity_chk_domain dom[MAX_DID];
};

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
		}
	}
}

uint64_t validate_guest_mapping_in_shadow(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t gpa, uint64_t size, uint64_t permit)
{
	uint64_t hpa_g, hpa_s, shadow_pml4, *pte, pte_size;
	int index = viommu->drhd_rt->index;
	struct acrn_vm *vm = viommu->vm;
	struct sanity_chk_domain *dom = &(sanity_chk_iommu_unit[index].dom[did]);

	dom->nr_verified++;

	if ((size != PTE_SIZE) && (size != PDE_SIZE) && (size != PDPTE_SIZE)) {
		pr_err("Invalid size:0x%x from guest mapping: DMAR%d, did:%d, iova:%llx", size, index, did, iova);
		return 0;
	}

	hpa_g = gpa2hpa(vm, gpa);
	if (hpa_g == INVALID_HPA) {
		if (iova == gpa) { /*RMRR Case: IOVA equals to GPA*/
			dom->nr_rmrr++;
			if (iova < dom->rmrr_bottom)
				dom->rmrr_bottom = iova;
			if (iova + size > dom->rmrr_top)
				dom->rmrr_top = iova + size;
		} else {
			dom->gpa2hpa_err++;
			if ((sanitty_chk_print) && (dom->gpa2hpa_err <= nr_max_print)) {
				pr_err("GPA2HPA Err: DMAR%d, did:%d, iova:%llx, gpa:%llx, guest pte_size:%lld", index, did, iova, gpa, size);
			}
			goto err;
		}
	}

	shadow_pml4 = get_shadow_pml4(viommu, did);
	pte = pgtable_lookup_entry((uint64_t *)shadow_pml4, iova, &pte_size,  &pgtable_ops);
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

					guest_did = GET_BITS(p_guest_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);
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
//	pr_err("%s, DMAR%d done %d context entries has been detected!\n", __func__, vtd->drhd_rt->index, ctx_cnt);
	return status;
}

static void dump_sanity_chk_info(struct acrn_viommu *viommu, uint16_t did, struct sanity_chk_domain *dom)
{
	int dmar_index = viommu->drhd_rt->index;

	if (dom->nr_verified == 0UL)
		return;

	pr_err("DMAR%d, DID: %-2d Entry Count: %-8lld PASS: %-8lld FAIL: %-8lld Shadow Map: %-8lld Shadow Unamp: %-8lld",
		dmar_index, did, dom->nr_verified, dom->hit_cnt, dom->nr_err, viommu->map_cnt[did], viommu->unmap_cnt[did]);

	if (dom->nr_rmrr > 0UL) {
		pr_err("=>RMRR Info: DMAR%d, DID: %-2d, RMRR Num: %lld, Bottom Addr:0x%llx, Top Addr: 0x%llx, Size: %lld(KB)",
			dmar_index, did, dom->nr_rmrr, dom->rmrr_bottom, dom->rmrr_top, (dom->rmrr_top - dom->rmrr_bottom) >> 10);
	}
	if (dom->nr_err > 0) {
		pr_err("=>ERROR Info: DMAR%d, DID: %-2d, Err Num: %lld, gpa2hpa Err:%lld, Shadow Missing: %lld, Addr Err: %lld, Size Err: %lld, Permit Err: %lld",
			dmar_index, did, dom->nr_err, dom->gpa2hpa_err, dom->miss_cnt, dom->addr_err, dom->size_err, dom->permit_err);
	}
}

static void check_shadow_pgtable(int dmar_index, uint16_t did, bool print_err, int max_print)
{
	int i, index;
	struct acrn_viommu *viommu;
	struct sanity_chk_domain *dom;

	reset_sanity_data();
	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		viommu = &vdmar_drhd_units[i];
		index = viommu->drhd_rt->index;
		if (!verify_guest_pml4_addr(viommu)) {
			return;
		}

		if (dmar_index == index) {
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
	dump_sanity_chk_info(viommu, did, dom);
}

static void check_shadow_pgtable_all(bool print_err, int max_print)
{
	uint32_t i, index;
	uint16_t did;
	struct acrn_viommu *viommu;
	struct sanity_chk_domain *dom;

	reset_sanity_data();

	sanitty_chk_print = print_err;
	nr_max_print = max_print;

	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		viommu = &vdmar_drhd_units[i];
		index = viommu->drhd_rt->index;
		if (!verify_guest_pml4_addr(viommu)) {
			return;
		}

		for (did = 0; did < 128; did++) {
			if ((viommu->shadow_pml4[did] != 0UL) && (viommu->guest_pml4[did] != 0UL)) {
				walk_guest_pgtable(viommu, did, validate_guest_mapping_in_shadow);
			}
		}
	}

	for (i = 0; i < plat_dmar_info.drhd_count; i++) {
		viommu = &vdmar_drhd_units[i];
		index = viommu->drhd_rt->index;
		for (did = 0; did < 128; did++) {
			if (viommu->guest_pml4[did] != 0UL) {
				dom = &(sanity_chk_iommu_unit[index].dom[did]);
				dump_sanity_chk_info(viommu, did, dom);
			}
		}
	}
}

static uint32_t record_tbl[MAX_TBL][MAX_TIME_RCD];
static uint64_t record_index[MAX_TBL];
static bool record_index_overflow[MAX_TBL];
void insert_time(int tbl_i, uint64_t us)
{
	if (tbl_i >= MAX_TBL) {
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
	uint64_t i, j, sum[MAX_TBL], av[MAX_TBL], max[MAX_TBL], min[MAX_TBL];

	for (i = 0; i < MAX_TBL; i++) {
		min[i] = (uint64_t)-1;
		max[i] = 0UL;
		sum[i] = 0UL;
		av[i] = 0UL;
	}

	for (i = 0; i < MAX_TBL; i++) {
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

	if (record_index_overflow[H_TBL_MAP] || record_index_overflow[H_TBL_UNMAP] ||record_index_overflow[H_IOTLB_PSI]) {
		pr_err("Index May Overflow in %s: %s, %s: %s, %s: %s",
			tbl_name[H_TBL_MAP], record_index_overflow[H_TBL_MAP] ? "TRUE" : "FALSE",
			tbl_name[H_TBL_UNMAP], record_index_overflow[H_TBL_UNMAP] ? "TRUE" : "FALSE",
			tbl_name[H_IOTLB_PSI], record_index_overflow[H_IOTLB_PSI] ? "TRUE" : "FALSE");

	} else if ((record_index[H_TBL_MAP] > 0) && (record_index[H_TBL_UNMAP] > 0) && (record_index[H_IOTLB_PSI] > 0)) {
		pr_err("Map Percent: %lld%%, Umap Percent: %lld%%.", (sum[H_TBL_MAP] * 100)/sum[H_IOTLB_PSI], (sum[H_TBL_UNMAP] * 100)/sum[H_IOTLB_PSI]);
	} else {
		pr_err("Info is NOT enough to get time consumed by Map & Unmap.");
	}

	for (i = 0; i < MAX_TBL; i++) {
		if (record_index[i] > 0UL)
			pr_err("%s: Record Index: %lld, av: %lld (us), max: %lld, min: %lld", tbl_name[i], record_index[i], av[i], max[i], min[i]);
	}
}

#define GUEST_MAPPING_LOOKUP		0 /* full param list.*/
#define SHADOW_MAPPING_LOOKUP		1 /* full param list*/
#define SHOW_SHADOW_TBL_ADDR		2 /* op only */
#define DUMP_GUEST_CONTEXT_TBL		3 /* op only */
#define DUMP_HOST_CONTEXT_TBL		4 /* op only */
#define MAP_UNMAP_CNT			5 /* op only */
#define READ_HOST_IOMMU_REG		7 /* op, dmar_index, did = 0, offset = addr, size = nr_pages*/
#define CHECK_SHADOW_MAPPING	 	8 /* op  dmar_index, did, print_err = (addr != 0UL)*/
#define CHECK_SHADOW_MAPPING_ALL	9 /* op  print_err = (dmar_index != 0) */
#define SHOW_TIME_COST			10 /* op */
void viommu_debug(uint64_t op, uint64_t dmar_index, uint64_t did, uint64_t addr, uint64_t nr_pages)
{
	uint32_t i, j, loop = 0;
	struct acrn_viommu *vtd;
	uint64_t pml4, size = 0;
	uint64_t *pte;
	uint64_t addr_end = addr + (nr_pages << 12);
	struct dmar_drhd_rt *iommu = NULL;
	uint64_t value;
	uint32_t offset;

//	pr_err("%s INPUT: op:%d, dmar_index:%d, did:%d, addr:0x%llx, nr_pages:%d", __func__, op, dmar_index, did, addr, nr_pages);
	if (op == READ_HOST_IOMMU_REG) {
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &vdmar_drhd_units[i];
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
			vtd = &vdmar_drhd_units[i];
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
			pte = pgtable_lookup_entry_d((uint64_t *)pml4, addr, &size,  &pgtable_ops);
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
			vtd = &vdmar_drhd_units[i];
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
			vtd = &vdmar_drhd_units[i];
			dump_root_table(get_guest_rta(vtd), vtd->drhd_rt->index);
		}
		return;
	}

	if (op == DUMP_HOST_CONTEXT_TBL) {
		pr_err("Dump Host Context:");
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &vdmar_drhd_units[i];
			dump_root_table(iommu_read64(vtd->drhd_rt, DMAR_RTADDR_REG), vtd->drhd_rt->index);
		}
		return;
	}

	if (op == MAP_UNMAP_CNT) {
		pr_err("Dump Shadow Map and Unmap Counter:");
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &vdmar_drhd_units[i];
			for (j = 0; j < MAX_GUEST_IOMMU_DID; j++) {
				if ((vtd->map_cnt[j] != 0UL) || (vtd->unmap_cnt[i] != 0UL))
					pr_err("DMAR%d, DID:%3d, Map count:%8lld, Unmap count:%8lld.",
						vtd->drhd_rt->index, j, vtd->map_cnt[j], vtd->unmap_cnt[j]);
			}

		}
		return;
	}

	if (op == CHECK_SHADOW_MAPPING) {
		check_shadow_pgtable(dmar_index, did, addr != 0UL, nr_pages /*max print message*/);
		return;
	}

	if (op == CHECK_SHADOW_MAPPING_ALL) {
		check_shadow_pgtable_all(dmar_index != 0, did /*max_print message*/);
		return;
	}

	if (op == SHOW_TIME_COST) {
		time_sum();
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
#endif //SHADOW_DBG
