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


/* TODO: every DMAR in every guest should have one vIOMMU */
static struct acrn_viommu vdmar_drhd_units[MAX_DRHDS] = {0};

static bool cc_req;
#define GET_BITS  dmar_get_bitslice

int dump_root_table(uint64_t rta, int dmar_index);
#define VIOMMU_SHADOW_PGTABLE_SIZE (2 << 20) //todo
static inline uint64_t io_pgentry_present(uint64_t pte)
{
	return pte & EPT_RWX;
}

static inline uint64_t shadow_pgentry_present(uint64_t pte)
{
	return pte & EPT_RWX;
}

static inline void shadow_clflush_pagewalk(const void* etry)
{
//	iommu_flush_cache(etry, sizeof(uint64_t));
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

	table->default_access_right = EPT_RWX;
	table->pgentry_present = shadow_pgentry_present;
	table->clflush_pagewalk = shadow_clflush_pagewalk;
	table->large_page_support = shadow_large_page_support;
	table->tweak_exe_right = shadow_nop_tweak_exe_right;
	table->recover_exe_right = shadow_nop_recover_exe_right;
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

	pr_err("%s: BDF [%lx:%lx:%lx]: DID:0x%x, TT:%llx, AW:%llx, FPD:%llx, P:%d, SL-PTPTR: 0x%llx.", str, bus, dev, fun, did, tt, aw, fpd, P, slptptr);
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
	pr_err("%s, DMAR%d done %d context entries has been detected!\n", __func__, dmar_index, ctx_cnt);
	return 0;
}


static uint64_t viommu_get_guest_root_table_addr(struct acrn_viommu *vdmar)
{
	return vdmar->guest_root_tbl_addr;
}

struct dmar_entry *viommu_get_native_context_entry(struct acrn_viommu *vtd, union pci_bdf *vbdf)
{
	uint32_t i;
	union pci_bdf pbdf;
	struct acrn_vm *vm = vtd->vm;
	struct acrn_vpci *vpci = &(vm->vpci);
	struct pci_vdev *vdev;//pci_vdevs[CONFIG_MAX_PCI_DEV_NUM];
	uint64_t native_rta = vtd->drhd_rt->root_table_addr;
	struct dmar_entry *p_rta, *p_root_e, *p_context, *p_context_e;

	p_rta = (struct dmar_entry *)vtd->drhd_rt->root_table_addr;

//	pr_err("%s enter, vBDF = [%x:%x:%x].", __func__, vbdf->bits.b, vbdf->bits.d, vbdf->bits.f);

	//use pci_find_vdev()
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
			/*Todo:check P bit of *p_root_e */
			if ((p_root_e->lo_64 & 0x1) == 0) {
				//pr_err("%s, NOT Present: native root entry for bus %d.", __func__, pbdf.fields.bus, p_root_e->lo_64);
				break;
			} else {
//				pr_err("%s, native root entry for bus %d is %llx.", __func__, pbdf.fields.bus, p_root_e->lo_64);
				//dump_root_entry(p_root_e);
			}

			p_context = (struct dmar_entry *)(p_root_e->lo_64 & (~0xFFF));

			p_context_e = p_context + pbdf.fields.devfun;
			if ((p_context_e->lo_64 & 0x1) == 0) {
				//pr_err("%s, NOT Present: native context entry for %x:%x:%x is not present.", __func__, pbdf.bits.b, pbdf.bits.d, pbdf.bits.f);
				return NULL;
			} else {
				//pr_err("%s, native context entry for pBDF: %x:%x:%x is:", __func__, pbdf.bits.b, pbdf.bits.d, pbdf.bits.f);
				//dump_context_entry("get-native-ctx-entry", pbdf.bits.b, pbdf.bits.d, pbdf.bits.f, p_context_e);
			}
			/*Todo: check P of this entry.*/
			return p_context_e;
		}
	}
	return NULL;
}

#define REMAPPED_DID_OFFSET 11 /* Low 11 bits are reserved for guest DID, mask:0x7FF */
static uint32_t construct_virtual_did(uint32_t vmid, uint32_t vdid)
{
	return ((1 << 15) | (vmid << REMAPPED_DID_OFFSET) | vdid);
}

int viommu_context_device_selective_invalidate(struct acrn_viommu *vtd, uint32_t did, uint32_t sid)
{
	int status = 0;
	uint16_t bus, devfun;
	uint16_t guest_did, remapped_did;
	uint64_t shadow_pml4, guest_pml4;
	uint16_t i, j, ctx_cnt = 0;
	struct dmar_entry *guest_root_e;
	union pci_bdf vbdf;
	struct dmar_entry *p_guest_context_e, *p_native_context_e, dummy_ctx_e;

	bus = (sid >> 8) & 0xFF;
	devfun = sid & 0xFF;
	guest_root_e = (struct dmar_entry *)(vtd->guest_root_tbl_addr & (~0xFFF)) + bus;
	if (guest_root_e->lo_64 & 0x1) {
		p_guest_context_e = (struct dmar_entry *) (guest_root_e->lo_64 & (~0xFFF)) + devfun;
		if (p_guest_context_e->lo_64 & 0x1) {
			//dump_context_entry("Guest-CTX-e", i, (j >> 3) & 0x1f, j & 0x7, p_guest_context_e);
			//pr_err("%s, ctx_cnt:%lld.", __func__, ctx_cnt);

			guest_did = GET_BITS(p_guest_context_e->hi_64, CTX_ENTRY_UPPER_DID_MASK, CTX_ENTRY_UPPER_DID_POS);
			ASSERT(guest_did < MAX_GUEST_IOMMU_DID, "Guest DID overflow");

			//if (((cc_inv_g == VTD_INV_DESC_CC_DOMAIN) || (cc_inv_g == VTD_INV_DESC_CC_DEVICE)) && (did != guest_did))
			//	continue;

			//pr_err("%s, CC_device: did:%lld, sid:[%x:%x:%x]", __func__, VTD_INV_DESC_CC_DID(entry->lo_64), (sid >> 8) & 0xff, (sid >> 3) &0x1f, sid & 0x7);
			//if ((cc_inv_g == VTD_INV_DESC_CC_DEVICE) && ((did != guest_did) || (((sid >> 8) & 0xFF ) != i) || ((sid & 0xFF) != j)))
			//if ((cc_inv_g == VTD_INV_DESC_CC_DEVICE) && ((((sid >> 8) & 0xFF ) != i) || ((sid & 0xFF) != j)))
			//	continue;

			/*
			 * VT-d specification #9.3, Context-entries programmed with the same domain identifier
			 * must always reference same address traslation(SLPTPTR field).
			 */
			if (vtd->shadow_pml4[guest_did] == 0UL) {
				/* create IOMMU shadow table for this guest IOMMU domain */
				shadow_pml4 = (uint64_t)pgtable_create_root(&vtd->shadow_pgtable);
				if (shadow_pml4 == 0UL) {
					goto exit;
				}
				pr_err("%s, create shadow table for guest vDMAR%d DID=%d, shadow PML4: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, shadow_pml4);
				vtd->shadow_pml4[guest_did] = shadow_pml4;
			} else {
				shadow_pml4 = vtd->shadow_pml4[guest_did];
			}

			if (vtd->guest_pml4_gpa[guest_did] == 0UL) {
				guest_pml4 = p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
				if (guest_pml4 != 0UL) {
					vtd->guest_pml4_gpa[guest_did] = guest_pml4;//p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
					//pr_err("%s, cache guest PML4 vDMAR%d DID=%d, guest PML4: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, guest_pml4);//vtd->guest_pml4_gpa[did]);
				} else {
					pr_err("%s, WARNING: Guest PML4 is NULL. vDMAR%d DID=%d, ctx entry: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, p_guest_context_e->lo_64);
				}
			}

			vbdf.fields.bus = bus;
			vbdf.fields.devfun = devfun;
			p_native_context_e = viommu_get_native_context_entry(vtd, &vbdf);
			if (p_native_context_e != NULL) {
				/* overwrite native context entry */
				#if 0//(SHADOW_EN == 0) //debug only
				//#error "No shadow!"
				memcpy_s(&dummy_ctx_e, sizeof(struct dmar_entry), p_native_context_e, sizeof(struct dmar_entry));
				p_native_context_e = &dummy_ctx_e;
				//dump_context_entry("Navtive CTX", vbdf.bits.b, vbdf.bits.d, vbdf.bits.f, p_native_context_e);
				//dump_context_entry("Guest   CTX", i, (j >> 3) & 0x1f, j & 0x7, p_guest_context_e);
				#endif

				p_native_context_e->lo_64 = p_guest_context_e->lo_64;
				p_native_context_e->lo_64 &= (~CTX_ENTRY_LOWER_SLPTPTR_MASK);
				p_native_context_e->lo_64 |= (shadow_pml4 & CTX_ENTRY_LOWER_SLPTPTR_MASK);

				remapped_did = construct_virtual_did(vtd->vm->vm_id, guest_did);
				p_native_context_e->hi_64 = p_guest_context_e->hi_64;
				p_native_context_e->hi_64 &= (~CTX_ENTRY_UPPER_DID_MASK);
				p_native_context_e->hi_64 |= ((remapped_did << CTX_ENTRY_UPPER_DID_POS) & CTX_ENTRY_UPPER_DID_MASK);
					//dump_context_entry("Shadow  CTX", vbdf.bits.b, vbdf.bits.d, vbdf.bits.f, p_native_context_e);
					//pr_err("\n");
			} else {
				pr_err("%s, fail to get native context entry, vBDF= [%x:%x:%x]", __func__, vbdf.bits.b, vbdf.bits.d, vbdf.bits.f);
				status = -1;
				goto exit;
			}
#if 0
			if (cc_inv_g == VTD_INV_DESC_CC_DEVICE) {
				/*
				 * Todo: split this function: one is for global context flush and the other is for device selective case,
				 * for device selective case, no need to loop and constant time to identify the entry for both guest table and guest table.
				 */
				goto exit;
			}
#endif
		}
	}
exit:
	return status;
}

int viommu_walk_through_guest_context_tables(struct acrn_viommu *vtd, uint64_t guest_rta, uint32_t cc_inv_g, uint32_t did, uint32_t sid)
{
#if 1
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
					if ((cc_inv_g == VTD_INV_DESC_CC_DEVICE) && ((((sid >> 8) & 0xFF ) != i) || ((sid & 0xFF) != j)))
						continue;

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
						pr_err("%s, create shadow table for guest vDMAR%d DID=%d, shadow PML4: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, shadow_pml4);
						vtd->shadow_pml4[guest_did] = shadow_pml4;
					} else {
						shadow_pml4 = vtd->shadow_pml4[guest_did];
					}

					if (vtd->guest_pml4_gpa[guest_did] == 0UL) {
						guest_pml4 = p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
						if (guest_pml4 != 0UL) {
							vtd->guest_pml4_gpa[guest_did] = guest_pml4;//p_guest_context_e->lo_64 & CTX_ENTRY_LOWER_SLPTPTR_MASK;
//							pr_err("%s, cache guest PML4 vDMAR%d DID=%d, guest PML4: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, guest_pml4);//vtd->guest_pml4_gpa[did]);
						} else {
							pr_err("%s, WARNING: Guest PML4 is NULL. vDMAR%d DID=%d, ctx entry: 0x%llx", __func__, vtd->drhd_rt->index, guest_did, p_guest_context_e->lo_64);
						}
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

						remapped_did = construct_virtual_did(vtd->vm->vm_id, guest_did);
						p_native_context_e->hi_64 = p_guest_context_e->hi_64;
						p_native_context_e->hi_64 &= (~CTX_ENTRY_UPPER_DID_MASK);
						p_native_context_e->hi_64 |= ((remapped_did << CTX_ENTRY_UPPER_DID_POS) & CTX_ENTRY_UPPER_DID_MASK);
						//dump_context_entry("Shadow  CTX", vbdf.bits.b, vbdf.bits.d, vbdf.bits.f, p_native_context_e);
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

void *viommu_get_guest_pml4(struct acrn_viommu *vtd, uint16_t did)
{
	return (void *) (vtd->guest_pml4_gpa[did]);
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

#if 0
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
				//pr_err("Remove: DMAR%d, did:%d, guest_pml4:%llx, shadow_pml4:%llx, iova:0x%llx, size:0x%lx, did:%d, loop:%d.\n",
				//vtd->drhd_rt->index, did, guest_pml4, shadow_pml4, iova, shadow_size, prot, did, loop);

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
			//shadow_pte = pgtable_lookup_entry(shadow_pml4, iova, &shadow_size, &pgtable_ops);
			//if (shadow_pte == NULL) { //add mapping to shadow
				//map from guest i/o page table , hence to add this mapping to shadow page table
				gpa = (((*guest_pte & (~EPT_PFN_HIGH_MASK)) & (~(guest_size - 1UL))) | (iova & (guest_size - 1UL)));
				hpa = gpa2hpa(vtd->vm, gpa);
				prot = EPT_RWX;

				#if 0
				pr_err("Add: DMAR%d, did:%d, guest_pml4:%llx, shadow_pml4:%llx, iova:%llx, hpa: 0x%llx, gpa:0x%llx, size: 0x%lx, loop:%d.",
					vtd->drhd_rt->index, did, guest_pml4, shadow_pml4, iova, hpa, gpa, guest_size, loop);
				#endif

				viommu_shadow_add_mr(vtd, shadow_pml4, hpa, iova, guest_size, prot);
				vtd->map_cnt[did]++;
				req_size = guest_size;
			#if 0
			} else {
				status = -1;
				pr_err("%s, WARNING: mapping of iova:0x%llx, Present in both guest[%llx] and shadow[%llx], loop:%d.", __func__, iova, *guest_pte, *shadow_pte, loop);
				break;
			}
			#endif

		}
		iova += req_size;
		loop++;
	}
	return status;
#endif


}

int viommu_shadow_page_table_sync(struct acrn_viommu *vtd, struct dmar_entry *entry)
{
	uint64_t page_addr, ih, did, am, lo_64, hi_64;
	struct dmar_drhd_rt *drhd_rt;
	struct dmar_entry *ctp;
	static reentry = true;
	static uint64_t succ;
	int status;
	uint32_t pages;
	uint64_t *guest_pml4;
	uint64_t *shadow_pml4;
	//static uint64_t dmarbits;
	int index = vtd->drhd_rt->index;

	if (!reentry)
		return -1;


	did = GET_BITS(entry->lo_64, IOTLB_INV_LOWER_DID_MASK, IOTLB_INV_LOWER_DID_POS);
	//pr_err("%s, CC_DOMAIN: did:%lld, my DID:%lld\n", __func__, VTD_INV_DESC_CC_DID(entry->lo_64), did);
	am = GET_BITS(entry->hi_64, IOTLB_INV_UPPER_AM_MASK, IOTLB_INV_UPPER_AM_POS);
	ih = GET_BITS(entry->hi_64, IOTLB_INV_UPPER_IH_MASK, IOTLB_INV_UPPER_IH_POS);
	page_addr = GET_BITS(entry->hi_64, IOTLB_INV_UPPER_ADDR_MASK, IOTLB_INV_UPPER_ADDR_POS);

	pages = 1 << am;
	page_addr <<= 12;

	if (did >= MAX_GUEST_IOMMU_DID) {
		pr_err("%s, Can't support guest did:%d.\n", __func__, did);
		return -1;
	}

	guest_pml4 = viommu_get_guest_pml4(vtd, did); /*todo GPA -> HVA*/
	shadow_pml4 = (uint64_t *)vtd->shadow_pml4[did];
	if ((guest_pml4 != NULL) && (shadow_pml4 != NULL)) {
		status = sync_shadow(vtd, guest_pml4, shadow_pml4, page_addr, pages, did);
		if (status) {
			pr_err("%s, sync shadow failed.\n", __func__);
			reentry = false;
		}
	}
	else {
		pr_err("%s, DMAR%d: Invalid guest pml4:0x%llx shadow pml4:0x%llx, did:%d.\n", __func__, index, guest_pml4, shadow_pml4, did);
		reentry = false;
	}

	return 0;
}

static void process_context_cache_desc(struct acrn_viommu *vdmar_unit, struct dmar_entry *entry)
{
	uint16_t sid = 0U, did = 0U;
	uint64_t cc_g = entry->lo_64 & VTD_INV_DESC_CC_G;

	/* Figure 6-20. Context-cache Invalidate Descriptor */
	did = VTD_INV_DESC_CC_DID(entry->lo_64);
	sid = VTD_INV_DESC_CC_SID(entry->lo_64);
	switch (cc_g) {
	case VTD_INV_DESC_CC_GLOBAL:
		/* On Linux, the translation table should be empty at this moment, just passthru this write */
	//	pr_err("%s, CC_global.", __func__);
		break;
	case VTD_INV_DESC_CC_DOMAIN:
		pr_err("%s, CC_DOMAIN, did:%lld.", __func__, VTD_INV_DESC_CC_DID(entry->lo_64));
		break;
	case VTD_INV_DESC_CC_DEVICE:
		//pr_err("%s, CC_device: did:%lld, sid:[%x:%x:%x]", __func__, VTD_INV_DESC_CC_DID(entry->lo_64), (sid >> 8) & 0xff, (sid >> 3) &0x1f, sid & 0x7);
		//entry->lo_64 = (entry->lo_64 & ~VTD_INV_DESC_CC_G) | VTD_INV_DESC_CC_DOMAIN;
		break;
	default:
		break;
	}
#if SHADOW_EN
	viommu_walk_through_guest_context_tables(vdmar_unit, vdmar_unit->guest_root_tbl_addr, cc_g, did, sid);
#endif
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
#if SHADOW_EN
		viommu_shadow_page_table_sync(vdmar_unit, entry);
#endif
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
//			dmar_issue_qi_request(dmar_unit, *entry);
			process_context_cache_desc(vdmar_unit, entry);
			//guest root table
			//dump_root_table(vdmar_unit->guest_root_tbl_addr, vdmar_unit->drhd_rt->index);

			// host root table
			//dump_root_table(dmar_unit->root_table_addr, dmar_unit->index);
			cc_req = true;
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
	#if 0
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
	#else
		value = dmar_unit->cap;
		value |= (1UL << 7U);
	#endif
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

	case DMAR_RTADDR_REG:
		if (vdmar_unit->guest_root_tbl_addr != 0UL) {
			pr_err("%s, guest is RE-set root addr: %llx, orig:%llx!!\n", __func__,
			mmio->value,
			vdmar_unit->guest_root_tbl_addr);
		}

		vdmar_unit->guest_root_tbl_addr = mmio->value; 
		pr_err("%s, guest is trying to set root addr: %llx\n", __func__, mmio->value);
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

void get_guest_map_unmap(void)
{
	uint32_t i, j;
	struct acrn_viommu *vtd;
	bool shadow_en;

#if (SHADOW_EN == 1)
	shadow_en = true;	
#else
	shadow_en = false;
#endif
	pr_err("Dump map and unmap count, IOMMU shadow talbe IS %sEnabled!", shadow_en? "" : "NOT ");
	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		vtd = &vdmar_drhd_units[i];
		for (j = 0; j < MAX_GUEST_IOMMU_DID; j++) {
			if (vtd->shadow_pml4[j] != 0UL)
				pr_err("DMAR%d, DID:%d, map_cnt:%lld, unmap_cnt:%lld.", i, j, vtd->map_cnt[j], vtd->unmap_cnt[j]);
		}
		pr_err("\n");
	}
}

void list_shadow_table(void)
{
	uint32_t i, j;
	struct acrn_viommu *vtd;
	bool shadow_en;

#if (SHADOW_EN == 1)
	shadow_en = true;	
#else
	shadow_en = false;
#endif

	pr_err("IOMMU shadow talbe IS %sEnabled!", shadow_en? "" : "NOT ");

	#if 1
	pr_err("Dump host context::");
	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		vtd = &vdmar_drhd_units[i];
		dump_root_table(vtd->drhd_rt->root_table_addr, vtd->drhd_rt->index);

	}
	#endif

	stac();
	#if 0
	pr_err("Dump guest context::");
	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		vtd = &vdmar_drhd_units[i];
		dump_root_table(vtd->guest_root_tbl_addr, vtd->drhd_rt->index);
	}
	#endif

	#if 0
	pr_err("Generate Shadow PML4:");
	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		vtd = &vdmar_drhd_units[i];
		viommu_walk_through_guest_context_tables(vtd, vtd->guest_root_tbl_addr,0,0,0);

	}
	#endif
	clac();

	pr_err("Dump shadow PML4:");
	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {
		vtd = &vdmar_drhd_units[i];
		for (j = 0; j < MAX_GUEST_IOMMU_DID; j++)
		{
			if (vtd->shadow_pml4[j] != 0UL)
				pr_err("DMAR%d, PML4[%d]:0x%llx.", vtd->drhd_rt->index, j, vtd->shadow_pml4[j]); 
		}

	}
}
#define G_PG 0
#define S_PG 1

void check_viommu_mapping(uint64_t op, uint64_t dmar_index, uint64_t did, uint64_t addr)
{
	uint32_t i;
	struct acrn_viommu *vtd;
	uint64_t pml4, size;
	uint64_t *pte;

	pr_err("%s, op:%d, dmar_index:%d, did:%d, addr:0x%llx", __func__, op, dmar_index, did, addr);

	for (i = 0U; i < plat_dmar_info.drhd_count; i++) {

		vtd = &vdmar_drhd_units[i];
		if (dmar_index == vtd->drhd_rt->index)
			break;
	}

	if (did >= MAX_GUEST_IOMMU_DID) {
		pr_err("%s, invalid did:%d", __func__, did);
		return;
	}

	if (op == G_PG) {
		pml4 = vtd->guest_pml4_gpa[did];
	} else if (op == S_PG){
		pml4 = vtd->shadow_pml4[did];
	} else { 
		pr_err("%s, invalid op:%d", __func__, op);
	}


	if (pml4 == 0UL) {
		pr_err("%s, pml4 is null, did:%d", __func__, did);
		return 0;
	}
		

	pte = pgtable_lookup_entry_d(pml4, addr, &size,  &pgtable_ops);
	pr_err("DMAR%d, %sPageTable, mapping of %llx is %llx", dmar_index, op == 0? "Guest ": "Shadow ", addr, *pte); 
}
