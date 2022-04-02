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

extern int dbg_mapping;

void sanity_check_guest_pgtable(void);
typedef void (*shadow_pge_sync_handler)(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t *pgentry, uint64_t size);

/* TODO: every DMAR in every guest should have one vIOMMU */
static struct acrn_viommu vdmar_drhd_units[MAX_DRHDS] = {0};

#define GET_BITS  dmar_get_bitslice

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
#define VIOMMU_SHADOW_PGTABLE_SIZE (2 << 20) //todo
static inline uint64_t io_pgentry_present(uint64_t pte)
{
	return pte & EPT_RWX;
}
#if 0
void flush_cache_range1(const volatile void *p, uint64_t size)
{
	uint64_t i;

	for (i = 0UL; i < size; i += CACHE_LINE_SIZE) {
		clflushopt(p + i);
	}
}
#endif
static inline uint64_t shadow_pgentry_present(uint64_t pte)
{
	return pte & EPT_RWX;
}

static inline void shadow_clflush_pagewalk(const void* etry)
{
	iommu_flush_cache(etry, sizeof(uint64_t));
//	flush_cache_range1(etry, sizeof(uint64_t));
//	wbinvd();
}

static inline bool shadow_large_page_support(enum _page_table_level level, __unused uint64_t prot)
{
	return ((level == IA32E_PD) || (level == IA32E_PDPT));
}

static inline void shadow_nop_tweak_exe_right(uint64_t *entry __attribute__((unused))) {}
static inline void shadow_nop_recover_exe_right(uint64_t *entry __attribute__((unused))) {}

struct pgtable pgtable_ops = {
	0UL, /* uint64_t default_access_right;*/
	NULL, /* struct page_pool *pool; */
	NULL, /* bool (*large_page_support)(enum _page_table_level level, uint64_t prot); */
	io_pgentry_present, /* uint64_t (*pgentry_present)(uint64_t pte); */
	NULL, /* void (*clflush_pagewalk)(const void *p); */
	NULL, /* void (*tweak_exe_right)(uint64_t *entry); */
	NULL, /* void (*recover_exe_right)(uint64_t *entry); */
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
	table->pgentry_present = shadow_pgentry_present;
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
			if (!table->pgentry_present(*shadow_pml4e)) {
				continue;
			}
			for (j = 0UL; j < PTRS_PER_PDPTE; j++) {
				shadow_pdpte = pdpte_offset(shadow_pml4e, j << PDPTE_SHIFT);
				if (!table->pgentry_present(*shadow_pdpte) ||
				    is_leaf_ept_entry(*shadow_pdpte, IA32E_PDPT)) {
					continue;
				}
				for (k = 0UL; k < PTRS_PER_PDE; k++) {
					shadow_pde = pde_offset(shadow_pdpte, k << PDE_SHIFT);
					if (!table->pgentry_present(*shadow_pde) ||
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

/**
 * @pre pge != NULL && size > 0.
 */
void shadow_sync_leaf_page(struct acrn_viommu *viommu, uint16_t did, uint64_t iova, uint64_t *pge, uint64_t size)
{
#if 0
	uint64_t shadow_pml4;
	uint64_t base_iova;//, end_iova;

	pr_err("%s, DMAR%d, did:%d, *pge:%llx, size:0x%x", __func__, viommu->drhd_rt->index, did, *pge, size);

	if ((*pge & EPT_MT_MASK) != EPT_UNCACHED) {
		base_iova = (*pge & (~(size - 1UL)));
		//end_iova = base_iova + size;
		viommu_shadow_table_psi_sync(viommu, did, base_iova, size); //(struct acrn_viommu *viommu, uint32_t did, uint64_t addr, uint64_t size)
	}
#else
	//pr_err("%s, DMAR%d, did:%d, iova:0x%llx, *pge:%llx, size:0x%x", __func__, viommu->drhd_rt->index, did, iova, *pge, size);

	viommu_shadow_table_psi_sync(viommu, did, iova, size); //(struct acrn_viommu *viommu, uint32_t did, uint64_t addr, uint64_t size)
#endif
}

/**
 * @pre vm != NULL && cb != NULL.
 */
void walk_guest_io_pgtable(struct acrn_viommu *viommu, uint16_t did, shadow_pge_sync_handler cb)
{
	const struct pgtable *table = &pgtable_ops;//&vm->arch_vm.ept_pgtable;
	uint64_t *pml4e, *pdpte, *pde, *pte;
	uint64_t i, j, k, m;
	uint64_t iova;
	uint64_t guest_pml4 = viommu->guest_pml4_gpa[did];
	uint64_t n1g = 0, n2m = 0, n4k = 0;

	//pr_err("%s DMAR%d, did:%d, start...", __func__, viommu->drhd_rt->index, did);

	for (i = 0UL; i < PTRS_PER_PML4E; i++) {
		//pml4e = pml4e_offset((uint64_t *)get_eptp(vm), i << PML4E_SHIFT);
		pml4e = pml4e_offset((uint64_t *)guest_pml4, i << PML4E_SHIFT);
		if (table->pgentry_present(*pml4e) == 0UL) {
			continue;
		}
		for (j = 0UL; j < PTRS_PER_PDPTE; j++) {
			pdpte = pdpte_offset(pml4e, j << PDPTE_SHIFT);
			if (table->pgentry_present(*pdpte) == 0UL) {
				continue;
			}
			if (pdpte_large(*pdpte) != 0UL) {
				iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT);
				cb(viommu, did, iova, pdpte, PDPTE_SIZE);
				n1g++;
				continue;
			}
			for (k = 0UL; k < PTRS_PER_PDE; k++) {
				pde = pde_offset(pdpte, k << PDE_SHIFT);
				if (table->pgentry_present(*pde) == 0UL) {
					continue;
				}
				if (pde_large(*pde) != 0UL) {
					iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT) | (k << PDE_SHIFT);
					n2m++;
					cb(viommu, did, iova, pde, PDE_SIZE);
					continue;
				}
				for (m = 0UL; m < PTRS_PER_PTE; m++) {
					pte = pte_offset(pde, m << PTE_SHIFT);
					iova = (i << PML4E_SHIFT) | (j << PDPTE_SHIFT) | (k << PDE_SHIFT) | (m << PTE_SHIFT);
					if (table->pgentry_present(*pte) != 0UL) {
						n4k++;
						cb(viommu, did, iova, pte, PTE_SIZE);
					}
				}
			}
		}
	}
	//pr_err("%s done, i = %lld,  j = %lld, k = %lld, nr_1G:%d, nr_2m:%d, nr_4k:%d.", __func__, i, j, k, n1g, n2m, n4k);
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
	uint16_t did;
	int i, j, ctx_cnt = 0;
	struct dmar_entry *root_entry, *ctp;
	static uint32_t bitmap;

	pr_err("%s, DMAR%d: RTA: 0x%llx", __func__, dmar_index, rta);
	root_entry = (struct dmar_entry *)(rta & (~0xFFF));
	for (i = 0; i < 3; i++) { //bus
		if (root_entry[i].lo_64 & 1) {
			dump_root_entry("dmup-root-e", i, &root_entry[i]);
			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & (~0xfff));
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

void *viommu_get_guest_pml4(struct acrn_viommu *vtd, uint16_t did)
{
	return (void *) (vtd->guest_pml4_gpa[did]);
}

static void reset_host_context_table(struct acrn_viommu *viommu)
{
	uint64_t rta;
	int i, j;
	struct dmar_entry *root_entry, *ctp;

	rta = viommu->drhd_rt->root_table_addr;
//	pr_err("%s, DMAR%d: RTA: 0x%llx", __func__, dmar_index, rta);
	root_entry = (struct dmar_entry *)(rta & (~0xFFF));
	for (i = 0; i < 3; i++) { //bus
		if (root_entry[i].lo_64 & 1) {
			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & (~0xfff));
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

			p_context = (struct dmar_entry *)(p_root_e->lo_64 & (~0xFFF));

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

static uint64_t viommu_get_guest_rta(struct acrn_viommu *viommu)
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
	struct dmar_entry *p_guest_context_e, *p_shadow_context_e, dummy_ctx_e;

	bus = (sid >> 8) & 0xFF;
	devfun = sid & 0xFF;
	guest_root_e = (struct dmar_entry *)(vtd->guest_root_tbl_addr & (~0xFFF)) + bus;
	if (guest_root_e->lo_64 & 0x1) {
		p_guest_context_e = (struct dmar_entry *) (guest_root_e->lo_64 & (~0xFFF)) + devfun;
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

			if (vtd->guest_pml4_gpa[guest_did] == 0UL) {
				vtd->guest_pml4_gpa[guest_did] = guest_pml4;//p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
				//pr_err("%s, Store guest PML4 vDMAR%d DID=%d, guest PML4: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, guest_pml4);//vtd->guest_pml4_gpa[did]);
			} else if (guest_pml4 != vtd->guest_pml4_gpa[guest_did]) {
				pr_err("%s, Guest is trying to update pml4 for did:%d, current guest pml4:0x%llx, cached guest pml4:0x%llx,",
					__func__, guest_did, guest_pml4, vtd->guest_pml4_gpa[guest_did]);
					vtd->guest_pml4_gpa[guest_did] = guest_pml4;
			}

			vbdf.fields.bus = bus;
			vbdf.fields.devfun = devfun;
			p_shadow_context_e = get_shadow_context_entry(vtd, &vbdf);
			if (p_shadow_context_e != NULL) {
				/* overwrite native context entry */
				#if 0//(SHADOW_EN == 0) //debug only
				//#error "No shadow!"
				memcpy_s(&dummy_ctx_e, sizeof(struct dmar_entry), p_shadow_context_e, sizeof(struct dmar_entry));
				p_shadow_context_e = &dummy_ctx_e;
				//dump_context_entry("Navtive CTX", vbdf.bits.b, vbdf.bits.d, vbdf.bits.f, p_shadow_context_e);
				//dump_context_entry("Guest   CTX", i, (j >> 3) & 0x1f, j & 0x7, p_guest_context_e);
				#endif

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
				//dump_context_entry("Shadow  CTX", vbdf.bits.b, vbdf.bits.d, vbdf.bits.f, p_shadow_context_e);
				//pr_err("\n");
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

	guest_rta = viommu_get_guest_rta(vtd);
	reset_host_context_table(vtd);
	root_entry = (struct dmar_entry *)(guest_rta & (~0xFFF));
	for (i = 0; i < 256; i++) { //bus
		if (root_entry[i].lo_64 & 1) {
			//dump_root_entry("Guest-root-e", i, &root_entry[i]);
			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & (~0xfff));
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

#if 0
int viommu_walk_through_guest_context_tables(struct acrn_viommu *vtd, uint64_t guest_rta, uint32_t cc_inv_g, uint32_t did, uint32_t sid)
{
#if 0
	int status = 0;
	uint32_t i, j;
	struct dmar_entry *root_entry, *ctp;
	struct dmar_entry *p_guest_context_e;

	if (cc_inv_g == VTD_INV_DESC_CC_DEVICE)
	{
		status = viommu_context_device_selective_invalidate(vtd, did, sid);
	} else {
		root_entry = (struct dmar_entry *)(vtd->guest_root_tbl_addr & (~0xFFF));
		for (i = 0; i < 256; i++) { //bus
			if ((root_entry[i].lo_64 & 1) == 0)
				continue;

			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & (~0xfff));
			for (j = 0; j < 256; j++) {//df
				p_guest_context_e = &ctp[j];
				if ((p_guest_context_e->lo_64 & 1) == 0)
					continue;

				status = viommu_context_device_selective_invalidate(vtd, did, (i << 8) | (j & 0xFF));
				if (status)
					goto exit;
			}
		}
	}

exit:
	return status;

#else
//works block
	int status = 0;
	uint16_t guest_did, remapped_did;
	uint64_t shadow_pml4, guest_pml4;
	uint16_t i, j, ctx_cnt = 0;
	struct dmar_entry *root_entry, *ctp;
	static uint32_t bitmap;
	union pci_bdf vbdf;
	struct dmar_entry *p_guest_context_e, *p_native_context_e, dummy_ctx_e;

	//pr_err("%s, DMAR%d: RTA: 0x%llx", __func__, vtd->drhd_rt->index, guest_rta);
	root_entry = (struct dmar_entry *)(guest_rta & (~0xFFF));
	for (i = 0; i < 3; i++) { //bus
		if (root_entry[i].lo_64 & 1) {
			//dump_root_entry("Guest-root-e", i, &root_entry[i]);
			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & (~0xfff));
			for (j = 0; j <= 255; j++) {//df
				p_guest_context_e = &ctp[j];
				if (p_guest_context_e->lo_64 & 1) {
					ctx_cnt++;

					//dump_context_entry("Guest-CTX-e", i, (j >> 3) & 0x1f, j & 0x7, p_guest_context_e);
					//pr_err("%s, ctx_cnt:%lld.", __func__, ctx_cnt);

					guest_did = GET_BITS(p_guest_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);
					ASSERT(guest_did < MAX_GUEST_IOMMU_DID, "Guest DID overflow");

					//if (((cc_inv_g == VTD_INV_DESC_CC_DOMAIN) || (cc_inv_g == VTD_INV_DESC_CC_DEVICE)) && (did != guest_did))
					//	continue;

					//pr_err("%s, CC_device: did:%lld, sid:[%x:%x:%x]", __func__, VTD_INV_DESC_CC_DID(entry->lo_64), (sid >> 8) & 0xff, (sid >> 3) &0x1f, sid & 0x7);
					//if ((cc_inv_g == VTD_INV_DESC_CC_DEVICE) && ((did != guest_did) || (((sid >> 8) & 0xFF ) != i) || ((sid & 0xFF) != j)))
					if ((cc_inv_g == VTD_INV_DESC_CC_DEVICE) && ((((sid >> 8) & 0xFF ) != i) || ((sid & 0xFF) != j))) {
						//pr_err("%s, SKIP: DMAR%d, CC_device: did:%lld, sid:[%x:%x:%x]", __func__, vtd->drhd_rt->index, VTD_INV_DESC_CC_DID(entry->lo_64), (sid >> 8) & 0xff, (sid >> 3) &0x1f, sid & 0x7);
						continue;
					}

					/*
					 * VT-d specification #9.3, Context-entries programmed with the same domain identifier
					 * must always reference same address traslation(SLPTPTR field).
					 */
					if (vtd->shadow_pml4[guest_did] == 0UL) {
						/* create IOMMU shadow table for this guest IOMMU domain */
						shadow_pml4 = (uint64_t)pgtable_create_root(&vtd->shadow_pgtable);
						if (shadow_pml4 == 0UL) {
							break;
						}
						vtd->shadow_pml4[guest_did] = shadow_pml4;
						if (0 && dbg_mapping)
							pr_err("%s, create shadow table for guest vDMAR%d DID=%d, shadow PML4: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, shadow_pml4);
					} else {
						shadow_pml4 = vtd->shadow_pml4[guest_did];
					}

					guest_pml4 = p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
					if (vtd->guest_pml4_gpa[guest_did] == 0UL) {
						if (guest_pml4 != 0UL) {
							vtd->guest_pml4_gpa[guest_did] = guest_pml4;//p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
						//	pr_err("%s, Store guest PML4 vDMAR%d DID=%d, guest PML4: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, guest_pml4);//vtd->guest_pml4_gpa[did]);
						} else {
							pr_err("%s, WARNING: Guest PML4 is NULL. vDMAR%d DID=%d, ctx entry: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, p_guest_context_e->lo_64);
						}
					} else if (guest_pml4 != vtd->guest_pml4_gpa[guest_did]) {
						pr_err("%s, WARNING: Guest is trying to update pml4 for did:%d.", __func__, guest_did);
					}

					vbdf.fields.bus = i;
					vbdf.fields.devfun = j;
					p_native_context_e = viommu_get_native_context_entry(vtd, &vbdf);
					if (p_native_context_e != NULL) {
						/* overwrite native context entry */
						#if (SHADOW_EN == 0) //debug only
						#error "No shadow!"
						memcpy_s(&dummy_ctx_e, sizeof(struct dmar_entry), p_native_context_e, sizeof(struct dmar_entry));
						p_native_context_e = &dummy_ctx_e;
						//dump_context_entry("Navtive CTX", vbdf.bits.b, vbdf.bits.d, vbdf.bits.f, p_native_context_e);
						//dump_context_entry("Guest   CTX", i, (j >> 3) & 0x1f, j & 0x7, p_guest_context_e);
						#endif

						p_native_context_e->lo_64 = p_guest_context_e->lo_64;
						p_native_context_e->lo_64 &= (~CTX_ENTRY_LOWER_SLPTPTR_MASK);
						p_native_context_e->lo_64 |= (shadow_pml4 & CTX_ENTRY_LOWER_SLPTPTR_MASK);
						//p_native_context_e->lo_64 |= (guest_pml4 & CTX_ENTRY_LOWER_SLPTPTR_MASK);

						remapped_did = construct_virtual_did(vtd->vm->vm_id, guest_did);
						p_native_context_e->hi_64 = p_guest_context_e->hi_64;
						p_native_context_e->hi_64 &= (~CTX_ENTRY_UPPER_DID_MASK);
						p_native_context_e->hi_64 |= ((remapped_did << CTX_ENTRY_UPPER_DID_POS) & CTX_ENTRY_UPPER_DID_MASK);
						//dump_context_entry("Shadow  CTX", vbdf.bits.b, vbdf.bits.d, vbdf.bits.f, p_native_context_e);
						if ( 1 && dbg_mapping)
							pr_err("Shadow vDMAR%d, did = %d,  BDF = %x:%x:%x.", vtd->drhd_rt->index, guest_did, vbdf.bits.b, vbdf.bits.d, vbdf.bits.f);

						//pr_err("\n");
					} else {
						pr_err("%s, fail to get native context entry, vBDF= [%x:%x:%x]", __func__, vbdf.bits.b, vbdf.bits.d, vbdf.bits.f);
						status = -1;
						goto exit;
					}

					if (cc_inv_g == VTD_INV_DESC_CC_DEVICE) {
						/*
						 * Todo: split this function: one is for global context flush and the other is for device selective case,
						 * for device selective case, no need to loop and constant time to identify the entry for both guest table and guest table.
						 */
						goto exit;
					}

				}
			}
		}
	}

exit:
//	pr_err("%s, DMAR%d done %d context entries has been detected!\n", __func__, vtd->drhd_rt->index, ctx_cnt);
	return status;
#endif
}

uint64_t get_iova_mapping_block(struct acrn_viommu *vtd, uint64_t *guest_pml4,  uint64_t iova_start, uint32_t max_size, uint64_t *size)
{
	uint64_t guest_size;
	const uint64_t *guest_pte;
	uint64_t hpa, gpa;
	uint64_t iova_end = iova_start + max_size, iova = iova_start;
	int loop = 0;
	uint64_t map_hpa_base = 0, map_hpa_size = 0;


	while (iova < iova_end) {
		guest_pte = pgtable_lookup_entry(guest_pml4, iova, &guest_size, &pgtable_ops);
		if (guest_pte != NULL) {
			gpa = (((*guest_pte & (~EPT_PFN_HIGH_MASK)) & (~(guest_size - 1UL))) | (iova & (guest_size - 1UL)));
			hpa = gpa2hpa(vtd->vm, gpa);
			if (map_hpa_base == 0UL) {
				map_hpa_base = hpa;
				map_hpa_size = guest_size;
				loop++;
			} else if (hpa == (map_hpa_base + map_hpa_size)) {
				map_hpa_size += guest_size;
				loop++;
				if (map_hpa_size >= max_size) {
					break;
				}
			} else {
				break;
			}

			iova += guest_size;
		} else {
			//pr_err("%s, fail to get mapping for iova:%llx, loop:%d.", __func__, iova, loop);
			break;
		}
	}

//	pr_err("%s, continouus block num:%d.", __func__, loop);
	*size = map_hpa_size;
	return map_hpa_base;
}

void check_mapping(uint64_t *pml4_page, uint64_t addr,
		uint64_t *pg_size, const struct pgtable *table)
{
	uint64_t size;

	pr_err("%s: pml4_page:%llx, addr:%llx", __func__, pml4_page, addr);
	uint64_t *pte = pgtable_lookup_entry_d(pml4_page, addr,	&size, table);
	pr_err("%s done: pte:llx", __func__, *pte);
}

int sync_shadow(struct acrn_viommu *vtd, uint64_t *guest_pml4, uint64_t *shadow_pml4, uint64_t addr, uint32_t nr_pages, uint32_t did)
{
	int status = 0;
	uint64_t guest_size, shadow_size, req_size;
	const uint64_t *guest_pte, *shadow_pte;
	uint64_t gpa, hpa, prot;
	uint64_t iova_end = addr + (nr_pages << 12), iova = addr;
	int loop = 0;
	bool iova_map =false, iova_unmap = false;
	uint64_t map_hpa_size;
	int index = vtd->drhd_rt->index;

#if 1
	if (dbg_mapping)
		pr_err("%s: DMAR%d, did:%d, guest pml4:%llx, shadow pml4:%llx, iova:%llx, iova_end:%llx, pages:%d.",
			__func__, vtd->drhd_rt->index, did,  guest_pml4, shadow_pml4, addr, iova_end, nr_pages);
#endif

#if 0
	guest_pte = pgtable_lookup_entry(guest_pml4, iova, &guest_size, &pgtable_ops);
	if (guest_pte == NULL) {
		//pr_err("Remove: iova:0x%llx, size:0x%lx, did:%d, loop:%d.\n", iova, shadow_size, prot, did, loop);
		viommu_shadow_del_mr(vtd, shadow_pml4, iova, nr_pages << 12);
		vtd->unmap_cnt[did]++;
	} else { /*mapping is present in guest.*/
		//shadow_pte = pgtable_lookup_entry(shadow_pml4, iova, &shadow_size, &pgtable_ops);
		 //mapping to shadow
		 if (nr_pages == 1) {
			gpa = (((*guest_pte & (~EPT_PFN_HIGH_MASK)) & (~(guest_size - 1UL))) | (iova & (guest_size - 1UL)));
			hpa = gpa2hpa(vtd->vm, gpa);
			viommu_shadow_add_mr(vtd, shadow_pml4, hpa, iova, guest_size, EPT_RWX);
			vtd->map_cnt[did]++;
		 } else {
			while(iova < iova_end) {
				map_hpa_size = 0;
				hpa = get_iova_mapping_block(vtd, guest_pml4, iova, (iova_end - iova), &map_hpa_size);
				if (map_hpa_size > 0) {
					vtd->map_cnt[did]++;
					viommu_shadow_add_mr(vtd, shadow_pml4, hpa, iova, map_hpa_size, EPT_RWX);
					iova += map_hpa_size;
				} else {
				//	status = -1;
					break;
				}
			}
		}
	}

	return status;
#else

	while (iova < iova_end) {
		guest_size = 0;
		shadow_size = 0;
		guest_pte = pgtable_lookup_entry(guest_pml4, iova, &guest_size, &pgtable_ops);
		if (guest_pte == NULL) {
			shadow_pte = pgtable_lookup_entry(shadow_pml4, iova, &shadow_size, &pgtable_ops);
			if(shadow_pte != NULL) {
				//unmap from guest i/o page table , hence to remove this mapping from shadow page table
				if (dbg_mapping && (iova == 0xfffbf000)) {	
					pr_err("Remove: DMAR%d, did:%d, guest_pml4:%llx, shadow_pml4:%llx, iova:0x%llx, size:0x%lx, did:%d, loop:%d.\n",
						vtd->drhd_rt->index, did, guest_pml4, shadow_pml4, iova, shadow_size, prot, did, loop);
				}

				viommu_shadow_del_mr(vtd, shadow_pml4, iova, shadow_size);
				req_size = shadow_size;
				vtd->unmap_cnt[did]++;
			} else {
			#if 0
				status = -1;
				pr_err("Remove Err[NULL]: DMAR%d, did:%d, guest_pml4:%llx, shadow_pml4:%llx, iova:0x%llx, Nothing in Both, nr_pages:%d, loop:%d.",
					vtd->drhd_rt->index, did, guest_pml4, shadow_pml4, iova, nr_pages, loop);
				check_mapping(shadow_pml4, iova, &guest_size, &pgtable_ops);
				//req_size = shadow_size;
			#endif
				break;
			}
		} else { /*mapping is present in guest.*/
			shadow_pte = pgtable_lookup_entry(shadow_pml4, iova, &shadow_size, &pgtable_ops);
			if (shadow_pte == NULL) { //add mapping to shadow
				//map from guest i/o page table , hence to add this mapping to shadow page table
				gpa = (((*guest_pte & (~EPT_PFN_HIGH_MASK)) & (~(guest_size - 1UL))) | (iova & (guest_size - 1UL)));
				hpa = gpa2hpa(vtd->vm, gpa);
				//prot = 0x3;//EPT_RWX;
				prot = (*guest_pte) & 0x7;

				viommu_shadow_add_mr(vtd, shadow_pml4, hpa, iova, guest_size, prot);
				if (dbg_mapping && (iova == 0xfffbf000)) {	
					pr_err("Add: DMAR%d, did:%d, guest_pml4:%llx, shadow_pml4:%llx, iova:%llx, hpa: 0x%llx, gpa:0x%llx, size: 0x%lx, loop:%d, nr_pages:%d.",
						vtd->drhd_rt->index, did, guest_pml4, shadow_pml4, iova, hpa, gpa, guest_size, loop, nr_pages);
						shadow_pte = pgtable_lookup_entry(shadow_pml4, iova, &shadow_size, &pgtable_ops);
					if (shadow_pte != NULL)
						pr_err("%s, iova:0x%llx is present in Shadow, pte:%llx", __func__, iova, *shadow_pte);
					else 
						pr_err("%s, iova:0x%llx is NOT present in Shadow.", __func__, iova);

				}

				vtd->map_cnt[did]++;
			#if 1
			} else {
				status = -1;
				pr_err("%s, WARNING: mapping of iova:0x%llx, Present in both guest[%llx] and shadow[%llx], loop:%d.", __func__, iova, *guest_pte, *shadow_pte, loop);
			//	break;
			}
			#endif
			req_size = guest_size;

		}
		iova += req_size;
		loop++;
	}
	return status;
#endif
}
#endif


int viommu_shadow_table_psi_sync(struct acrn_viommu *viommu, uint32_t did, uint64_t addr, uint64_t size)
{
	int status = 0;
	uint64_t guest_size, shadow_size, req_size, back_size;
	const uint64_t *guest_pte, *shadow_pte, *shadow_pte_back;
	uint64_t gpa, hpa, prot;
	uint64_t iova_end = addr + size, iova = addr;
	int loop = 0;
	//bool iova_map =false, iova_unmap = false;
	uint64_t map_hpa_size;
	
	uint64_t guest_pml4 = viommu_get_guest_pml4(viommu, did); /*todo GPA -> HVA*/
	uint64_t shadow_pml4 = viommu->shadow_pml4[did];
	
	while (iova < iova_end) {
		guest_size = 0;
		shadow_size = 0;
		guest_pte = pgtable_lookup_entry(guest_pml4, iova, &guest_size, &pgtable_ops);
		if (guest_pte == NULL) {
			shadow_pte = pgtable_lookup_entry(shadow_pml4, iova, &shadow_size, &pgtable_ops);
			if(shadow_pte != NULL) {
				//unmap from guest i/o page table , hence to remove this mapping from shadow page table
				if (dbg_mapping & 0) {	
					pr_err("Remove: DMAR%d, did:%d, guest_pml4:%llx, shadow_pml4:%llx, iova:0x%llx, size:0x%lx, did:%d, loop:%d.\n",
						viommu->drhd_rt->index, did, guest_pml4, shadow_pml4, iova, shadow_size, did, loop);
				}

				viommu_shadow_del_mr(viommu, shadow_pml4, iova, shadow_size);
				req_size = shadow_size;
				viommu->unmap_cnt[did]++;
			} else {
			#if 0
				status = -1;
				pr_err("Remove Err[NULL]: DMAR%d, did:%d, guest_pml4:%llx, shadow_pml4:%llx, iova:0x%llx, Nothing in Both, nr_pages:%d, loop:%d.",
					viommu->drhd_rt->index, did, guest_pml4, shadow_pml4, iova, nr_pages, loop);
				//check_mapping(shadow_pml4, iova, &guest_size, &pgtable_ops);
				//req_size = shadow_size;
			#endif
				break;
			}
		} else { /*mapping is present in guest.*/
			shadow_pte = pgtable_lookup_entry(shadow_pml4, iova, &shadow_size, &pgtable_ops);
			if (shadow_pte == NULL) { //add mapping to shadow
				//map from guest i/o page table , hence to add this mapping to shadow page table
				gpa = (((*guest_pte & (~EPT_PFN_HIGH_MASK)) & (~(guest_size - 1UL))) | (iova & (guest_size - 1UL)));
				hpa = gpa2hpa(viommu->vm, gpa);
				if (hpa != INVALID_HPA) {
					prot = (*guest_pte) & EPT_RWX;

					viommu_shadow_add_mr(viommu, shadow_pml4, hpa, iova, guest_size, prot);
					if (dbg_mapping & 1) {	
						shadow_pte_back = pgtable_lookup_entry(shadow_pml4, iova, &back_size, &pgtable_ops);
						//pr_err("Add: DMAR%d, did:%d, guest_pml4:%llx, shadow_pml4:%llx, iova:%llx, hpa: 0x%llx, gpa:0x%llx, size: 0x%lx, loop:%d.",
						//	viommu->drhd_rt->index, did, guest_pml4, shadow_pml4, iova, hpa, gpa, guest_size, loop);
						if ((*shadow_pte_back & (~(guest_size -1UL))) != gpa) {// applicable to service VM only. 
							pr_err("Add Err: DMAR%d, did:%d, REQ: iova:%llx, gpa:%llx, hpa: %llx, ReadBack: hpa:%llx, size: 0x%lx.",
								viommu->drhd_rt->index, did, iova, gpa, hpa, *shadow_pte_back, back_size);
						}
					}
					viommu->map_cnt[did]++;
				} else {
					pr_err("%s, fail to get HPA for GPA:%llx, guest pte:%llx", __func__, gpa, *guest_pte);
				}

			#if 1
			} else {
				status = -1;
				pr_err("%s, WARNING: mapping of iova:0x%llx is  present in both guest[%llx] and shadow[%llx], loop:%d.", __func__, iova, *guest_pte, *shadow_pte, loop);
			//	break;
			}
			#endif
			req_size = guest_size;

		}
		iova += req_size;
		loop++;
	}
	return status;
}

static int iotlb_inv_domain(struct acrn_viommu *viommu, uint32_t did)
{
	uint64_t guest_pml4;
	uint64_t shadow_pml4;
	int index = viommu->drhd_rt->index;
	
	guest_pml4 = viommu_get_guest_pml4(viommu, did); /*todo GPA -> HVA*/
	shadow_pml4 = viommu->shadow_pml4[did];

	//unmap all.
	viommu_free_shadow_table(viommu, shadow_pml4);

	//pr_err("%s, DMAR%d: did:%d, guest_pml4:0x%llx, shadow_pml4:0x%llx.", __func__, index, did, guest_pml4, shadow_pml4);
	//walk_guest_io_pgtable(viommu, did, shadow_sync_leaf_page);
	
	return 0;	
}


static int iotlb_inv_global(struct acrn_viommu *viommu)
{
	int did, cnt = 0;
	int index = viommu->drhd_rt->index;
	uint64_t guest_pml4, shadow_pml4;
	
	for (did = 0; did < MAX_GUEST_IOMMU_DID; did++) {
		guest_pml4 = viommu_get_guest_pml4(viommu, did); /*Todo GPA -> HVA*/
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
//	int index = viommu->drhd_rt->index;

#if 0	
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
#endif	

	did = VTD_INV_DESC_IOTLB_DID(iotlb_inv_desc->lo_64);
	addr = VTD_INV_DESC_IOTLB_ADDR(iotlb_inv_desc->hi_64);
	am = VTD_INV_DESC_IOTLB_AM(iotlb_inv_desc->hi_64);
	size = ((1 << am) << 12);
	
	if (did >= MAX_GUEST_IOMMU_DID) {
		pr_err("%s, Can't support guest did:%d.\n", __func__, did);
		return -1;
	}

	guest_pml4 = viommu_get_guest_pml4(viommu, did); /*todo GPA -> HVA*/
	shadow_pml4 = viommu->shadow_pml4[did];
	if ((guest_pml4 != 0UL) && (shadow_pml4 != 0UL)) {
		status = viommu_shadow_table_psi_sync(viommu, did, addr, size);
		//cpu_write_memory_barrier();
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
		//sanity_check_guest_pgtable();
		break;

	case VTD_INV_DESC_CC_DOMAIN:
		pr_err("%s,DMAR%d, CC_Domain, did:%lld.", __func__, viommu->drhd_rt->index, VTD_INV_DESC_CC_DID(entry->lo_64));
		break;

	case VTD_INV_DESC_CC_DEVICE:
		did = VTD_INV_DESC_CC_DID(entry->lo_64); /* always be 0 from linux guest. */
		sid = VTD_INV_DESC_CC_SID(entry->lo_64);
		fm = VTD_INV_DESC_CC_FM(entry->lo_64);
		//pr_err("%s,DMAR%d, CC_Device, did:%lld.", __func__, viommu->drhd_rt->index, VTD_INV_DESC_CC_DID(entry->lo_64));
		pr_err("%s, DMAR%d, CC_Device: sid:[%x:%x:%x]", __func__,
			viommu->drhd_rt->index, (sid >> 8) & 0xff, (sid >> 3) &0x1f, sid & 0x7);
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
		pr_err("%s, DMAR%d, IOTLB_Domain, did:%d.", __func__, index, (entry->lo_64 >> 16) & 0xFFFF);

		/*guest page table maybe present when guest issue domain iotlb.*/
		iotlb_inv_domain(viommu,(entry->lo_64 >> 16) & 0xFFFF);
		//sanity_check_guest_pgtable();
		break;

	case VTD_INV_DESC_IOTLB_PAGE:
		if (!iommu_cap_max_amask_val(viommu->drhd_rt->cap)) {
			entry->lo_64 = (entry->lo_64 & ~VTD_INV_DESC_IOTLB_G) | VTD_INV_DESC_IOTLB_DOMAIN;
			entry->hi_64 = 0UL;
		}
		if (dbg_mapping) {
		#if 0
			pr_err("%s, DMAR%d, IOTLB_PSI, did:%d, iova:0x%llx, pages:%d.",
				__func__, index, (entry->lo_64 >> 16) & 0xFFFF, entry->hi_64 & (~0xfff), 1 << (entry->hi_64 & 0x3f));
		#endif
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
		}
		break;

	default:
		break;
	}

	return write_iqt;
}

static void handle_iqt_write(struct acrn_viommu *vdmar_unit, uint16_t tail)
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
			process_context_cache_desc(vdmar_unit, entry);
			write_iqt = true; 
			break;

		case DMAR_INV_IOTLB_DESC:
			write_iqt = process_iotlb_desc(vdmar_unit, entry);
			break;

		case DMAR_INV_WAIT_DESC:
		{
			if (dmar_issue_qi_complete(dmar_unit)) {
				/* set the Done status in the wait entry */
				uint32_t *status_ptr = (uint32_t *)gpa2hva(vdmar_unit->vm, entry->hi_64);
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

		head = (head + DMAR_QI_INV_ENTRY_SIZE) % DMAR_INVALIDATION_QUEUE_SIZE;
	}
	clac();
}

#define MAX_DMAR_REG_SPACE 0x1000
static uint64_t viommu_mmio_read(struct acrn_viommu *viommu, struct acrn_mmio_request *mmio)
{
	struct dmar_drhd_rt *iommu = viommu->drhd_rt;
	int index = iommu->index;
	uint32_t offset = mmio->address - iommu->drhd->reg_base_addr;
	uint64_t value;

	if (offset + mmio->size > MAX_DMAR_REG_SPACE) {
		pr_err("%s, DMAR%d offset: 0x%x, size: %d overflow.", __func__, iommu->index, offset, mmio->size);
		value = 0UL;
	}

	spinlock_obtain(&viommu->lock);

	switch (offset) {
	case DMAR_CAP_REG: /*todo: move to default case */
		value = viommu_read64(viommu, DMAR_CAP_REG);
		break;

	case DMAR_ECAP_REG:
		value = viommu_read64(viommu, DMAR_ECAP_REG); /*todo: move to default case */
		break;

	case DMAR_IQT_REG:
		//val2 = viommu_read64(viommu, DMAR_IQT_REG);
		value = viommu->qi_tail;
		break;

	case DMAR_IQH_REG:
		//val2 = viommu_read64(viommu, DMAR_IQH_REG);
		value = viommu->qi_head;
		break;

	case DMAR_IQA_REG:
		//val2 = viommu_read64(viommu, DMAR_IQA_REG);
		value = viommu->qi_queue;
		break;

	default:
		if (mmio->size == 4U) {
			value = iommu_read32(iommu, offset);
		} else {
			value = iommu_read64(iommu, offset);
		}
		//pr_err("%s, DMAR%d, Read from native: offset:0x%x, host value:0x%llx", __func__, index, offset, value);
	}

	spinlock_release(&viommu->lock);

	if ((offset != DMAR_FSTS_REG) || (value != 0U)) {
		dev_dbg(DBG_LEVEL_VIOMMU, "rd dmar%d offset %x size %x value %llx", iommu->index, offset, mmio->size, value);
	}

	/* Remove Interrupt remapping Enabled flag */
	if (offset == DMAR_GSTS_REG) {
		value &= viommu->gcmd;
	}

exit:
	return value;
}

int viommu_gcmd_handle(struct acrn_viommu *viommu, uint32_t gcmd)
{
#if 0
#define DMA_GCMD_TE (1U << 31U)
#define DMA_GCMD_SRTP (1U << 30U)
#define DMA_GCMD_SFL (1U << 29U)
#define DMA_GCMD_EAFL (1U << 28U)
#define DMA_GCMD_WBF (1U << 27U)
#define DMA_GCMD_QIE (1U << 26U)
#define DMA_GCMD_SIRTP (1U << 24U)
#define DMA_GCMD_IRE (1U << 25U)
#define DMA_GCMD_CFI (1U << 23U)
#endif
#define UNSUPPORTED_GCMD (DMA_GCMD_CFI | DMA_GCMD_IRE | DMA_GCMD_SIRTP | DMA_GCMD_EAFL | DMA_GCMD_SFL) 
int index = viommu->drhd_rt->index;

	if ((gcmd & UNSUPPORTED_GCMD) != 0U) { 
		pr_err("Unsupported GCMD: 0x%lx", gcmd);	
		return -1; 
	}
	
	if (gcmd & DMA_GCMD_QIE) {
		if (index == 5)
			pr_err("%s, DMAR%d, QIE.", __func__, viommu->drhd_rt->index);
	}

	if (gcmd & DMA_GCMD_WBF) {
		if (index == 5)
		pr_err("%s, DMAR%d, WBF.", __func__, viommu->drhd_rt->index);
	}

	if (gcmd & DMA_GCMD_SRTP) {
		if (index == 5)
		pr_err("%s, DMAR%d, SRTP.", __func__, viommu->drhd_rt->index);
	}

	if (gcmd & DMA_GCMD_TE) {
		if (index == 5)
		pr_err("%s, DMAR%d, TE.", __func__, viommu->drhd_rt->index);
	}

	return 0;
}


static void viommu_mmio_write(struct acrn_viommu *viommu, struct acrn_mmio_request *mmio)
{
	struct dmar_drhd_rt *dmar_unit = viommu->drhd_rt;
	uint32_t offset = mmio->address - dmar_unit->drhd->reg_base_addr;
	bool write_reg = true;

	if (offset + mmio->size > MAX_DMAR_REG_SPACE) {
		pr_err("%s, DMAR%d offset: 0x%x, size: %d overflow.", __func__, dmar_unit->index, offset, mmio->size);
		goto exit;
	}

	if (offset != DMAR_IQT_REG) {
		//if (dmar_unit->index == 5)
			//pr_err("%s --> DMAR%d offset: 0x%x, size: %d,  value: 0x%llx", __func__, dmar_unit->index, offset, mmio->size, mmio->value);
	}

	spinlock_obtain(&viommu->lock);

	switch (offset) {
	case DMAR_IQT_REG:
		if (viommu->gcmd & DMA_GCMD_QIE) {
			handle_iqt_write(viommu, mmio->value);
			viommu->qi_tail = (uint16_t)mmio->value;
			viommu->qi_head = viommu->qi_tail;
			viommu_write64(viommu, DMAR_IQT_REG, mmio->value);
			viommu_write64(viommu, DMAR_IQH_REG, mmio->value);
		} else {
			/*Todo: Inject execepton to guest when write IQT while QIE is not set.*/
			pr_err("%s, DMAR%d:  Can't write IQT if QIE is clear, gcmd:%lx", __func__, dmar_unit->index, viommu->gcmd);
		}
		write_reg = false;
		break;

	case DMAR_IQA_REG:
		viommu->qi_queue = (uint64_t)gpa2hva(viommu->vm, mmio->value);
		viommu->qi_head = 0U;
		viommu->qi_tail = 0U;
		viommu_write64(viommu, DMAR_IQA_REG, mmio->value);
		viommu_write64(viommu, DMAR_IQH_REG, 0UL);
		viommu_write64(viommu, DMAR_IQT_REG, 0UL);

		/* Don't write QI Addr register */
		write_reg = false;
		break;

	case DMAR_GCMD_REG:
	{
		uint32_t gsts = iommu_read32(dmar_unit, DMAR_GSTS_REG);
		viommu->gcmd = mmio->value;
		//if (dmar_unit->index == 5)
		//	pr_err("%s, DMAR%d, handle GCMD, gsts: 0x%x val: 0x%x", __func__, dmar_unit->index, gsts, mmio->value);
		//viommu_gcmd_handle(viommu, (uint32_t)mmio->value);

		/* Reset SRTP (bit30) and TE (bits31) since we write through
		 * the Root Table Address Register (Register Offset 020h) now.
		 */
		mmio->value = gsts;

		write_reg = false;
		//write_reg = true;
		//dev_dbg(3, "%s DMAR%d, gsts: 0x%x val: 0x%x", __func__, dmar_unit->index, gsts, mmio->value);
		break;
	}

	case DMAR_FECTL_REG:
	case DMAR_FEDATA_REG:
	case DMAR_FEADDR_REG:
	case DMAR_FEUADDR_REG:
		viommu_write32(viommu, offset, (uint32_t)mmio->value);
		/* Hypervisor owns the fault management */
		write_reg = false;
		break;

	case DMAR_RTADDR_REG:
		if (viommu->guest_root_tbl_addr != 0UL) {
			pr_err("%s, guest is RE-set root addr: %llx, orig:%llx!!\n", __func__,
			mmio->value,
			viommu->guest_root_tbl_addr);
		}

		viommu->guest_root_tbl_addr = mmio->value; 
		viommu_write64(viommu, offset, mmio->value);
		write_reg = false;
		break;

	default:
//		pr_err("%s, Unhandled Write offset:0x%x, value:0x%llx", __func__, offset, mmio->value);
		break;
	}

	if (write_reg) {
		//pr_err("%s, Write-thru offset:0x%x, value:0x%llx", __func__, offset, mmio->value);
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
	pr_err("%s, DMAR%d: VT-d VER:%lx (Major:bit7~4, Minor:bit3~0)",
		__func__, index, viommu_read64(viommu, DMAR_VER_REG));

	/* Capability */
	val64 = dmar_unit->cap;
	val64 &= (~(VTD_CAP_ESIRTPS | VTD_CAP_FL5LP | VTD_CAP_PI | VTD_CAP_FL1GP | VTD_CAP_AFL)); /* Always clear bits. */
	val64 |= (VTD_CAP_PSI | VTD_CAP_CM); /* Always set capability bits */
	//val64 |= (VTD_CAP_CM); /* Always set capability bits */
	viommu_write64(viommu, DMAR_CAP_REG, val64);
	pr_err("%s, DMAR%d: Host cap: %-16llx Guest cap: %-16llx", __func__,
		index, dmar_unit->cap, viommu_read64(viommu, DMAR_CAP_REG));

	/* Extend Capability */
	val64 = dmar_unit->ecap;
	/* Expose below extend capability bits only */
	val64 &= (VTD_ECAP_SC | VTD_ECAP_DT | VTD_ECAP_QI | VTD_ECAP_C);
	viommu_write64(viommu, DMAR_ECAP_REG, val64);
	pr_err("%s, DMAR%d: Host ecap: %-16llx Guest ecap: %-16llx", __func__,
		index, dmar_unit->ecap, viommu_read64(viommu, DMAR_ECAP_REG));
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


/*All below are debug code */
static uint64_t nr_mapping_miss;
void shadow_mapping_verify(struct acrn_viommu *viommu, uint16_t did, uint64_t addr, uint64_t *pge, uint64_t size)
{
	static uint64_t max_msg;
	uint64_t shadow_size, req_size;
	const uint64_t *shadow_pte;
	uint64_t iova_end = addr + size, iova = addr;
	uint64_t shadow_pml4 = viommu->shadow_pml4[did];

	while (iova < iova_end) {
		shadow_size = 0;
		shadow_pte = pgtable_lookup_entry(shadow_pml4, iova, &shadow_size, &pgtable_ops);
		if(shadow_pte == NULL) {
			pr_err("DMAR%d, did:%d, iova:0x%llx is NOT mapped in shadow, guest pte:0x%llx.\n",
				viommu->drhd_rt->index, did, iova, *pge);
			nr_mapping_miss++;
			req_size = 4096;
		} else {
			if (max_msg++ <= 200)
				pr_err("iova:0x%llx, gpa:0x%llx, hpa:0x%llx, size:0x%x", iova, *pge, *shadow_pte, shadow_size);

			req_size = shadow_size;
		}
		iova += req_size;
	}
}

void viommu_check_shadow_pgtable(int dmar_index)
{
	int i, index, num = 0;
	uint16_t did;
	bool found = false;
	struct acrn_viommu *viommu;

	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		viommu = &vdmar_drhd_units[i];
		index = viommu->drhd_rt->index;
		if (dmar_index == index) {
			found = true;
			break;
		}
	}

	if (!found) {
		pr_err("%s, not found DMAR%d.", __func__, dmar_index);
		return;
	}

	pr_err("%s, Walk guest shadow for DMAR%d...", __func__, dmar_index);
	nr_mapping_miss = 0UL;
	stac();
	for (did = 0; did < 128; did++) {
		if ((viommu->shadow_pml4[did] != 0UL) && (viommu->guest_pml4_gpa[did] != 0UL)) {
			walk_guest_io_pgtable(viommu, did, shadow_mapping_verify);
			num++;
		}
	}
	clac();
	pr_err("Walked %d tables for DMAR%d Done, missed mapping number: %lld(0 means possible mis-sync between cache and RAM).", num, dmar_index, nr_mapping_miss);
}

struct sanity_chk_domain {
	uint64_t hit_cnt;
	uint64_t miss_cnt;
};

struct sanity_chk_viommu {
	struct sanity_chk_domain dom[128];
};
struct sanity_chk_viommu sanity_chk_iommu_unit[8];

void validate_guest_mapping(struct acrn_viommu *viommu, uint16_t did, uint64_t addr, uint64_t *pte, uint64_t size)
{
	uint64_t gpa, hpa;
	int index = viommu->drhd_rt->index;
	struct acrn_vm *vm = viommu->vm;
	struct sanity_chk_domain *dom = &(sanity_chk_iommu_unit[viommu->drhd_rt->index].dom[did]);

	gpa = (((*pte & (~EPT_PFN_HIGH_MASK)) & (~(size - 1UL))) | (addr & (size - 1UL)));
	hpa = gpa2hpa(vm, gpa);
	if (hpa == INVALID_HPA) {
		dom->miss_cnt++;
		//pr_err("Invalid HPA for guest mapping: DMAR%d, did:%d, iova:%llx, gpa:%llx,  pte:%llx", index, did, addr, gpa, *pte);
	} else {
		dom->hit_cnt++;
	}
}

bool verify_guest_pml4_addr(struct acrn_viommu *viommu)
{
	bool status = true;
	uint16_t guest_did;
	uint64_t guest_pml4, guest_rta;
	uint16_t i, j;
	struct dmar_entry *root_entry, *ctp;
	struct dmar_entry *p_guest_context_e;

	guest_rta = viommu_get_guest_rta(viommu);
	root_entry = (struct dmar_entry *)(guest_rta & (~0xFFF));
	for (i = 0; i < 3; i++) { //bus
		if (root_entry[i].lo_64 & 1) {
			ctp = (struct dmar_entry *)(root_entry[i].lo_64 & (~0xfff));
			for (j = 0; j <= 255; j++) {//df
				p_guest_context_e = &ctp[j];
				if (p_guest_context_e->lo_64 & 1) {

					guest_did = GET_BITS(p_guest_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);
					guest_pml4 = p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;

					if (viommu->guest_pml4_gpa[guest_did] != guest_pml4) {
						pr_err("%s, Mismatch guest PML4 vDMAR%d DID=%d, guest PML4: 0x%llx, cached pml4:0x%llx",
							__func__, viommu->drhd_rt->index, guest_did, guest_pml4, viommu->guest_pml4_gpa[guest_did]);
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

void sanity_check_guest_pgtable(void)
{
	uint32_t i, j, index, num = 0;
	uint16_t did;
	bool found = false;
	struct acrn_viommu *viommu;
	struct sanity_chk_domain *dom;

	pr_err("%s ...", __func__);

	memset((void *)&sanity_chk_iommu_unit[0], 0, 8 * sizeof(struct sanity_chk_viommu));
	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		viommu = &vdmar_drhd_units[i];
		index = viommu->drhd_rt->index;
		if (!verify_guest_pml4_addr(viommu)) {
			return;
		}

		for (did = 0; did < 128; did++) {
			if ((viommu->shadow_pml4[did] != 0UL) && (viommu->guest_pml4_gpa[did] != 0UL)) {
				walk_guest_io_pgtable(viommu, did, validate_guest_mapping);
			}
		}
	}

	for (i = 0; i < plat_dmar_info.drhd_count; i++) {
		viommu = &vdmar_drhd_units[i];
		index = viommu->drhd_rt->index;
		for (did = 0; did < 128; did++) {
			if (viommu->guest_pml4_gpa[did] != 0UL) {
				dom = &(sanity_chk_iommu_unit[index].dom[did]);
				pr_err("DMAR%d, DID: %-2d, Totally %-8lld Entries Verified, PASS: %-8lld  FAIL: %-8lld Shadow Map: %-8lld Shadow Unamp: %-8lld",
					i, did, (dom->hit_cnt + dom->miss_cnt), dom->hit_cnt, dom->miss_cnt,
					viommu->map_cnt[did], viommu->unmap_cnt[did]);
			}
		}
	}
	pr_err("%s done", __func__);
}

#define GUEST_MAPPING_LOOKUP		0 /* full param list.*/
#define SHADOW_MAPPING_LOOKUP		1 /* full param list*/
#define SHOW_SHADOW_TBL_ADDR		2 /* op only */
#define DUMP_GUEST_CONTEXT_TBL		3 /* op only */
#define DUMP_HOST_CONTEXT_TBL		4 /* op only */
#define MAP_UNMAP_CNT			5 /* op only */
#define READ_HOST_IOMMU_REG		7 /* op, dmar_index, did = 0, offset = addr, size = nr_pages*/
#define SANITY_CHECK_GUEST_MAPPING 	8 /* op only*/
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
		for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
			vtd = &vdmar_drhd_units[i];
			if (dmar_index == vtd->drhd_rt->index)
				break;
		}
		if (op == GUEST_MAPPING_LOOKUP) {
			pml4 = vtd->guest_pml4_gpa[did];
		} else if (op == SHADOW_MAPPING_LOOKUP){
			pml4 = vtd->shadow_pml4[did];
		}
		if (pml4 == 0UL) {
			pr_err("%s, pml4 is null, did:%d", __func__, did);
			return 0;
		}

		while (addr < addr_end) {
			pr_err("pgcheck, addr:0x%llx, addr end:0x%llx.",addr, addr_end);
			pte = pgtable_lookup_entry_d(pml4, addr, &size,  &pgtable_ops);
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
				if ((vtd->guest_pml4_gpa[j] == 0UL) && (vtd->shadow_pml4[j] == 0UL)) {
					continue;
				} else if ((vtd->guest_pml4_gpa[j] != 0UL) && (vtd->shadow_pml4[j] != 0UL)) {
					pr_err("DMAR%d, Guest PML4 of DID[%03d]:0x%llx, Shadow PML4:0x%llx.", vtd->drhd_rt->index, j, vtd->guest_pml4_gpa[j], vtd->shadow_pml4[j]);
				} else {
					pr_err("Error: DMAR%d, Guest PML4 of DID[%03d]:0x%llx, Shadow PML4:0x%llx.", vtd->drhd_rt->index, j, vtd->guest_pml4_gpa[j], vtd->shadow_pml4[j]);
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
			dump_root_table(viommu_get_guest_rta(vtd), vtd->drhd_rt->index);
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

	if (op == SANITY_CHECK_GUEST_MAPPING) {
		sanity_check_guest_pgtable();
		return;
	}

	pr_err("%s, Unhandled op:%d", __func__, op);
	return;
}

