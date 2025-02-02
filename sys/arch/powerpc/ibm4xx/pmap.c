/*	$NetBSD: pmap.c,v 1.110 2024/02/01 22:02:18 andvar Exp $	*/

/*
 * Copyright 2001 Wasabi Systems, Inc.
 * All rights reserved.
 *
 * Written by Eduardo Horvath and Simon Burge for Wasabi Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed for the NetBSD Project by
 *      Wasabi Systems, Inc.
 * 4. The name of Wasabi Systems, Inc. may not be used to endorse
 *    or promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY WASABI SYSTEMS, INC. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL WASABI SYSTEMS, INC
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (C) 1995, 1996 Wolfgang Solfrank.
 * Copyright (C) 1995, 1996 TooLs GmbH.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by TooLs GmbH.
 * 4. The name of TooLs GmbH may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY TOOLS GMBH ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL TOOLS GMBH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: pmap.c,v 1.110 2024/02/01 22:02:18 andvar Exp $");

#ifdef _KERNEL_OPT
#include "opt_ddb.h"
#include "opt_pmap.h"
#endif

#include <sys/param.h>
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <sys/pool.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/systm.h>

#include <uvm/uvm.h>

#include <machine/powerpc.h>

#include <powerpc/pcb.h>

#include <powerpc/spr.h>
#include <powerpc/ibm4xx/spr.h>

#include <powerpc/ibm4xx/cpu.h>
#include <powerpc/ibm4xx/tlb.h>

/*
 * kernmap is an array of PTEs large enough to map in
 * 4GB.  At 16KB/page it is 256K entries or 2MB.
 */
#define KERNMAP_SIZE	((0xffffffffU / PAGE_SIZE) + 1)
void *kernmap;

#define MINCTX		2
#define NUMCTX		256

volatile struct pmap *ctxbusy[NUMCTX];

#define TLBF_USED	0x1
#define	TLBF_REF	0x2
#define	TLBF_LOCKED	0x4
#define	TLB_LOCKED(i)	(tlb_info[(i)].ti_flags & TLBF_LOCKED)

typedef struct tlb_info_s {
	char	ti_flags;
	char	ti_ctx;		/* TLB_PID assiciated with the entry */
	u_int	ti_va;
} tlb_info_t;

volatile tlb_info_t tlb_info[NTLB];
/* We'll use a modified FIFO replacement policy cause it's cheap */
volatile int tlbnext;

static int tlb_nreserved = 0;
static int pmap_bootstrap_done = 0;

/* Event counters */
struct evcnt tlbmiss_ev = EVCNT_INITIALIZER(EVCNT_TYPE_TRAP,
    NULL, "cpu", "tlbmiss");
struct evcnt tlbflush_ev = EVCNT_INITIALIZER(EVCNT_TYPE_TRAP,
    NULL, "cpu", "tlbflush");
struct evcnt tlbenter_ev = EVCNT_INITIALIZER(EVCNT_TYPE_TRAP,
    NULL, "cpu", "tlbenter");
EVCNT_ATTACH_STATIC(tlbmiss_ev);
EVCNT_ATTACH_STATIC(tlbflush_ev);
EVCNT_ATTACH_STATIC(tlbenter_ev);

struct pmap kernel_pmap_;
struct pmap *const kernel_pmap_ptr = &kernel_pmap_;

static int npgs;
static u_int nextavail;
#ifndef MSGBUFADDR
extern paddr_t msgbuf_paddr;
#endif

static struct mem_region *mem, *avail;

/*
 * This is a cache of referenced/modified bits.
 * Bits herein are shifted by ATTRSHFT.
 */
static char *pmap_attrib;

#define PV_WIRED	0x1
#define PV_WIRE(pv)	((pv)->pv_va |= PV_WIRED)
#define PV_UNWIRE(pv)	((pv)->pv_va &= ~PV_WIRED)
#define PV_ISWIRED(pv)	((pv)->pv_va & PV_WIRED)
#define PV_VA(pv)	((pv)->pv_va & ~PV_WIRED)
#define PV_CMPVA(va,pv)	(!(PV_VA(pv) ^ (va)))

struct pv_entry {
	struct pv_entry *pv_next;	/* Linked list of mappings */
	struct pmap *pv_pm;
	vaddr_t pv_va;			/* virtual address of mapping */
};

/* Each index corresponds to TLB_SIZE_* value. */
static size_t tlbsize[] = {
	1024, 		/* TLB_SIZE_1K */
	4096, 		/* TLB_SIZE_4K */
	16384, 		/* TLB_SIZE_16K */
	65536, 		/* TLB_SIZE_64K */
	262144, 	/* TLB_SIZE_256K */
	1048576, 	/* TLB_SIZE_1M */
	4194304, 	/* TLB_SIZE_4M */
	16777216, 	/* TLB_SIZE_16M */
};

struct pv_entry *pv_table;
static struct pool pv_pool;

static int pmap_initialized;

static void ctx_flush(int);

struct pv_entry *pa_to_pv(paddr_t);
static inline char *pa_to_attr(paddr_t);

static inline volatile u_int *pte_find(struct pmap *, vaddr_t);
static inline int pte_enter(struct pmap *, vaddr_t, u_int);

static inline int pmap_enter_pv(struct pmap *, vaddr_t, paddr_t, int);
static void pmap_remove_pv(struct pmap *, vaddr_t, paddr_t);

static inline void tlb_invalidate_entry(int);

static int ppc4xx_tlb_size_mask(size_t, int *, int *);


struct pv_entry *
pa_to_pv(paddr_t pa)
{
	uvm_physseg_t bank;
	psize_t pg;

	bank = uvm_physseg_find(atop(pa), &pg);
	if (bank == UVM_PHYSSEG_TYPE_INVALID)
		return NULL;
	return &uvm_physseg_get_pmseg(bank)->pvent[pg];
}

static inline char *
pa_to_attr(paddr_t pa)
{
	uvm_physseg_t bank;
	psize_t pg;

	bank = uvm_physseg_find(atop(pa), &pg);
	if (bank == UVM_PHYSSEG_TYPE_INVALID)
		return NULL;
	return &uvm_physseg_get_pmseg(bank)->attrs[pg];
}

/*
 * Insert PTE into page table.
 */
static inline int
pte_enter(struct pmap *pm, vaddr_t va, u_int pte)
{
	int seg = STIDX(va), ptn = PTIDX(va);
	u_int oldpte;

	if (!pm->pm_ptbl[seg]) {
		/* Don't allocate a page to clear a non-existent mapping. */
		if (!pte)
			return 0;

		vaddr_t km = uvm_km_alloc(kernel_map, PAGE_SIZE, 0,
		    UVM_KMF_WIRED | UVM_KMF_ZERO | UVM_KMF_NOWAIT);

		if (__predict_false(km == 0))
			return ENOMEM;

		pm->pm_ptbl[seg] = (u_int *)km;
	}
	oldpte = pm->pm_ptbl[seg][ptn];
	pm->pm_ptbl[seg][ptn] = pte;

	/* Flush entry. */
	ppc4xx_tlb_flush(va, pm->pm_ctx);
	if (oldpte != pte) {
		if (pte == 0)
			pm->pm_stats.resident_count--;
		else
			pm->pm_stats.resident_count++;
	}
	return 0;
}

/*
 * Get a pointer to a PTE in a page table.
 */
volatile u_int *
pte_find(struct pmap *pm, vaddr_t va)
{
	int seg = STIDX(va), ptn = PTIDX(va);

	if (pm->pm_ptbl[seg])
		return &pm->pm_ptbl[seg][ptn];

	return NULL;
}

/*
 * This is called during initppc, before the system is really initialized.
 */
void
pmap_bootstrap(u_int kernelstart, u_int kernelend)
{
	struct mem_region *mp, *mp1;
	int cnt, i;
	u_int s, e, sz;

	tlbnext = tlb_nreserved;

	/*
	 * Allocate the kernel page table at the end of
	 * kernel space so it's in the locked TTE.
	 */
	kernmap = (void *)kernelend;

	/*
	 * Initialize kernel page table.
	 */
	for (i = 0; i < STSZ; i++)
		pmap_kernel()->pm_ptbl[i] = NULL;
	ctxbusy[0] = ctxbusy[1] = pmap_kernel();

	/*
	 * Announce page-size to the VM-system
	 */
	uvmexp.pagesize = NBPG;
	uvm_md_init();

	/*
	 * Get memory.
	 */
	mem_regions(&mem, &avail);
	for (mp = mem; mp->size; mp++) {
		physmem += btoc(mp->size);
		printf("+%lx,", mp->size);
	}
	printf("\n");
	ppc4xx_tlb_init();
	/*
	 * Count the number of available entries.
	 */
	for (cnt = 0, mp = avail; mp->size; mp++)
		cnt++;

	/*
	 * Page align all regions.
	 * Non-page aligned memory isn't very interesting to us.
	 * Also, sort the entries for ascending addresses.
	 */
	kernelstart &= ~PGOFSET;
	kernelend = (kernelend + PGOFSET) & ~PGOFSET;
	for (mp = avail; mp->size; mp++) {
		s = mp->start;
		e = mp->start + mp->size;
		printf("%08x-%08x -> ", s, e);
		/*
		 * Check whether this region holds all of the kernel.
		 */
		if (s < kernelstart && e > kernelend) {
			avail[cnt].start = kernelend;
			avail[cnt++].size = e - kernelend;
			e = kernelstart;
		}
		/*
		 * Look whether this regions starts within the kernel.
		 */
		if (s >= kernelstart && s < kernelend) {
			if (e <= kernelend)
				goto empty;
			s = kernelend;
		}
		/*
		 * Now look whether this region ends within the kernel.
		 */
		if (e > kernelstart && e <= kernelend) {
			if (s >= kernelstart)
				goto empty;
			e = kernelstart;
		}
		/*
		 * Now page align the start and size of the region.
		 */
		s = round_page(s);
		e = trunc_page(e);
		if (e < s)
			e = s;
		sz = e - s;
		printf("%08x-%08x = %x\n", s, e, sz);
		/*
		 * Check whether some memory is left here.
		 */
		if (sz == 0) {
 empty:
			memmove(mp, mp + 1,
			    (cnt - (mp - avail)) * sizeof(*mp));
			cnt--;
			mp--;
			continue;
		}
		/*
		 * Do an insertion sort.
		 */
		npgs += btoc(sz);
		for (mp1 = avail; mp1 < mp; mp1++)
			if (s < mp1->start)
				break;
		if (mp1 < mp) {
			memmove(mp1 + 1, mp1, (char *)mp - (char *)mp1);
			mp1->start = s;
			mp1->size = sz;
		} else {
			mp->start = s;
			mp->size = sz;
		}
	}

	/*
	 * We cannot do pmap_steal_memory here,
	 * since we don't run with translation enabled yet.
	 */
#ifndef MSGBUFADDR
	/*
	 * allow for msgbuf
	 */
	sz = round_page(MSGBUFSIZE);
	mp = NULL;
	for (mp1 = avail; mp1->size; mp1++)
		if (mp1->size >= sz)
			mp = mp1;
	if (mp == NULL)
		panic("not enough memory?");

	npgs -= btoc(sz);
	msgbuf_paddr = mp->start + mp->size - sz;
	mp->size -= sz;
	if (mp->size <= 0)
		memmove(mp, mp + 1, (cnt - (mp - avail)) * sizeof(*mp));
#endif

	for (mp = avail; mp->size; mp++)
		uvm_page_physload(atop(mp->start), atop(mp->start + mp->size),
		    atop(mp->start), atop(mp->start + mp->size),
		    VM_FREELIST_DEFAULT);

	/*
	 * Initialize kernel pmap and hardware.
	 */
	/* Setup TLB pid allocator so it knows we alreadu using PID 1 */
	pmap_kernel()->pm_ctx = KERNEL_PID;
	nextavail = avail->start;

	pmap_bootstrap_done = 1;
}

/*
 * Restrict given range to physical memory
 *
 * (Used by /dev/mem)
 */
void
pmap_real_memory(paddr_t *start, psize_t *size)
{
	struct mem_region *mp;

	for (mp = mem; mp->size; mp++) {
		if (*start + *size > mp->start &&
		    *start < mp->start + mp->size) {
			if (*start < mp->start) {
				*size -= mp->start - *start;
				*start = mp->start;
			}
			if (*start + *size > mp->start + mp->size)
				*size = mp->start + mp->size - *start;
			return;
		}
	}
	*size = 0;
}

/*
 * Initialize anything else for pmap handling.
 * Called during vm_init().
 */
void
pmap_init(void)
{
	struct pv_entry *pv;
	vsize_t sz;
	vaddr_t addr;
	int bank, i, s;
	char *attr;

	sz = (vsize_t)((sizeof(struct pv_entry) + 1) * npgs);
	sz = round_page(sz);
	addr = uvm_km_alloc(kernel_map, sz, 0, UVM_KMF_WIRED | UVM_KMF_ZERO);

	s = splvm();

	pv = pv_table = (struct pv_entry *)addr;
	for (i = npgs; --i >= 0;)
		pv++->pv_pm = NULL;
	pmap_attrib = (char *)pv;
	memset(pv, 0, npgs);

	pv = pv_table;
	attr = pmap_attrib;
	for (bank = uvm_physseg_get_first(); uvm_physseg_valid_p(bank);
	     bank = uvm_physseg_get_next(bank)) {
		sz = uvm_physseg_get_end(bank) - uvm_physseg_get_start(bank);
		uvm_physseg_get_pmseg(bank)->pvent = pv;
		uvm_physseg_get_pmseg(bank)->attrs = attr;
		pv += sz;
		attr += sz;
	}

	pmap_initialized = 1;

	splx(s);

	/* Setup a pool for additional pvlist structures */
	pool_init(&pv_pool, sizeof(struct pv_entry), 0, 0, 0, "pv_entry",
	    NULL, IPL_VM);
}

/*
 * How much virtual space is available to the kernel?
 */
void
pmap_virtual_space(vaddr_t *start, vaddr_t *end)
{

	*start = (vaddr_t) VM_MIN_KERNEL_ADDRESS;
	*end = (vaddr_t) VM_MAX_KERNEL_ADDRESS;
}

#ifdef PMAP_GROWKERNEL
/*
 * Preallocate kernel page tables to a specified VA.
 * This simply loops through the first TTE for each
 * page table from the beginning of the kernel pmap,
 * reads the entry, and if the result is
 * zero (either invalid entry or no page table) it stores
 * a zero there, populating page tables in the process.
 * This is not the most efficient technique but i don't
 * expect it to be called that often.
 */
extern struct vm_page *vm_page_alloc1(void);
extern void vm_page_free1(struct vm_page *);

vaddr_t kbreak = VM_MIN_KERNEL_ADDRESS;

vaddr_t
pmap_growkernel(vaddr_t maxkvaddr)
{
	struct pmap *pm = pmap_kernel();
	paddr_t pg;
	int seg, s;

	s = splvm();

	/* Align with the start of a page table */
	for (kbreak &= ~(PTMAP - 1); kbreak < maxkvaddr; kbreak += PTMAP) {
		seg = STIDX(kbreak);

		if (pte_find(pm, kbreak))
			continue;

		if (uvm.page_init_done)
			pg = (paddr_t)VM_PAGE_TO_PHYS(vm_page_alloc1());
		else if (!uvm_page_physget(&pg))
			panic("pmap_growkernel: no memory");
		if (!pg)
			panic("pmap_growkernel: no pages");
		pmap_zero_page((paddr_t)pg);

		/* XXX This is based on all phymem being addressable */
		pm->pm_ptbl[seg] = (u_int *)pg;
	}

	splx(s);

	return kbreak;
}

/*
 *	vm_page_alloc1:
 *
 *	Allocate and return a memory cell with no associated object.
 */
struct vm_page *
vm_page_alloc1(void)
{
	struct vm_page *pg;

	pg = uvm_pagealloc(NULL, 0, NULL, UVM_PGA_USERESERVE);
	if (pg) {
		pg->wire_count = 1;	/* no mappings yet */
		pg->flags &= ~PG_BUSY;	/* never busy */
	}
	return pg;
}

/*
 *	vm_page_free1:
 *
 *	Returns the given page to the free list,
 *	disassociating it with any VM object.
 *
 *	Object and page must be locked prior to entry.
 */
void
vm_page_free1(struct vm_page *pg)
{

	KASSERTMSG(pg->flags == (PG_CLEAN | PG_FAKE),
	    "invalid page pg = %p, pa = %" PRIxPADDR,
	    pg, VM_PAGE_TO_PHYS(pg));

	pg->flags |= PG_BUSY;
	pg->wire_count = 0;
	uvm_pagefree(pg);
}
#endif

/*
 * Create and return a physical map.
 */
struct pmap *
pmap_create(void)
{
	struct pmap *pm;

	pm = kmem_alloc(sizeof(*pm), KM_SLEEP);
	memset(pm, 0, sizeof(*pm));
	pm->pm_refs = 1;
	return pm;
}

/*
 * Add a reference to the given pmap.
 */
void
pmap_reference(struct pmap *pm)
{

	pm->pm_refs++;
}

/*
 * Retire the given pmap from service.
 * Should only be called if the map contains no valid mappings.
 */
void
pmap_destroy(struct pmap *pm)
{
	int i;

	if (--pm->pm_refs > 0)
		return;
	KASSERT(pm->pm_stats.resident_count == 0);
	KASSERT(pm->pm_stats.wired_count == 0);
	for (i = 0; i < STSZ; i++)
		if (pm->pm_ptbl[i]) {
			uvm_km_free(kernel_map, (vaddr_t)pm->pm_ptbl[i],
			    PAGE_SIZE, UVM_KMF_WIRED);
			pm->pm_ptbl[i] = NULL;
		}
	if (pm->pm_ctx)
		ctx_free(pm);
	kmem_free(pm, sizeof(*pm));
}

/*
 * Copy the range specified by src_addr/len
 * from the source map to the range dst_addr/len
 * in the destination map.
 *
 * This routine is only advisory and need not do anything.
 */
void
pmap_copy(struct pmap *dst_pmap, struct pmap *src_pmap, vaddr_t dst_addr,
	  vsize_t len, vaddr_t src_addr)
{
}

/*
 * Require that all active physical maps contain no
 * incorrect entries NOW.
 */
void
pmap_update(struct pmap *pmap)
{
}

/*
 * Fill the given physical page with zeroes.
 */
void
pmap_zero_page(paddr_t pa)
{
	int i;

#ifdef PPC_4XX_NOCACHE
	memset((void *)pa, 0, PAGE_SIZE);
#else

	for (i = PAGE_SIZE/CACHELINESIZE; i > 0; i--) {
		__asm volatile ("dcbz 0,%0" : : "r" (pa));
		pa += CACHELINESIZE;
	}
#endif
}

/*
 * Copy the given physical source page to its destination.
 */
void
pmap_copy_page(paddr_t src, paddr_t dst)
{

	memcpy((void *)dst, (void *)src, PAGE_SIZE);
	dcache_wbinv_page(dst);
}

static inline int
pmap_enter_pv(struct pmap *pm, vaddr_t va, paddr_t pa, int flags)
{
	struct pv_entry *pv, *npv;
	int s;

	KASSERT(pmap_initialized);

	s = splvm();

	pv = pa_to_pv(pa);
	if (!pv->pv_pm) {
		/*
		 * No entries yet, use header as the first entry.
		 */
		pv->pv_va = va;
		pv->pv_pm = pm;
		pv->pv_next = NULL;
	} else {
		/*
		 * There is at least one other VA mapping this page.
		 * Place this entry after the header.
		 */
		npv = pool_get(&pv_pool, PR_NOWAIT);
		if (npv == NULL) {
			if ((flags & PMAP_CANFAIL) == 0)
				panic("pmap_enter_pv: failed");
			splx(s);
			return ENOMEM;
		}
		npv->pv_va = va;
		npv->pv_pm = pm;
		npv->pv_next = pv->pv_next;
		pv->pv_next = npv;
		pv = npv;
	}
	if (flags & PMAP_WIRED) {
		PV_WIRE(pv);
		pm->pm_stats.wired_count++;
	}

	splx(s);

	return 0;
}

static void
pmap_remove_pv(struct pmap *pm, vaddr_t va, paddr_t pa)
{
	struct pv_entry *pv, *npv;

	/*
	 * Remove from the PV table.
	 */
	pv = pa_to_pv(pa);
	if (!pv)
		return;

	/*
	 * If it is the first entry on the list, it is actually
	 * in the header and we must copy the following entry up
	 * to the header.  Otherwise we must search the list for
	 * the entry.  In either case we free the now unused entry.
	 */
	if (pm == pv->pv_pm && PV_CMPVA(va, pv)) {
		if (PV_ISWIRED(pv))
			pm->pm_stats.wired_count--;
		if ((npv = pv->pv_next)) {
			*pv = *npv;
			pool_put(&pv_pool, npv);
		} else
			pv->pv_pm = NULL;
	} else {
		for (; (npv = pv->pv_next) != NULL; pv = npv)
			if (pm == npv->pv_pm && PV_CMPVA(va, npv))
				break;
		if (npv) {
			pv->pv_next = npv->pv_next;
			if (PV_ISWIRED(npv)) {
				pm->pm_stats.wired_count--;
			}
			pool_put(&pv_pool, npv);
		}
	}
}

/*
 * Insert physical page at pa into the given pmap at virtual address va.
 */
int
pmap_enter(struct pmap *pm, vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{
	u_int tte;
	bool managed;
	int s;

	/*
	 * Have to remove any existing mapping first.
	 */
	pmap_remove(pm, va, va + PAGE_SIZE);

	if (flags & PMAP_WIRED)
		flags |= prot;

	managed = uvm_pageismanaged(pa);

	/*
	 * Generate TTE.
	 */
	tte = TTE_PA(pa);
	/* XXXX -- need to support multiple page sizes. */
	tte |= TTE_SZ_16K;

	KASSERT((flags & (PMAP_NOCACHE | PME_WRITETHROUG)) !=
	    (PMAP_NOCACHE | PME_WRITETHROUG));

	if (flags & PMAP_NOCACHE) {
		/* Must be I/O mapping */
		tte |= TTE_I | TTE_G;
	}
#ifdef PPC_4XX_NOCACHE
	tte |= TTE_I;
#else
	else if (flags & PME_WRITETHROUG) {
		/* Uncached and writethrough are not compatible */
		tte |= TTE_W;
	}
#endif

	if (pm == pmap_kernel())
		tte |= TTE_ZONE(ZONE_PRIV);
	else
		tte |= TTE_ZONE(ZONE_USER);

	if (flags & VM_PROT_WRITE)
		tte |= TTE_WR;

	if (flags & VM_PROT_EXECUTE)
		tte |= TTE_EX;

	/*
	 * Now record mapping for later back-translation.
	 */
	if (pmap_initialized && managed) {
		char *attr;

		if (pmap_enter_pv(pm, va, pa, flags)) {
			/* Could not enter pv on a managed page */
			return ENOMEM;
		}

		/* Now set attributes. */
		attr = pa_to_attr(pa);
		KASSERT(attr);
		if (flags & VM_PROT_ALL)
			*attr |= PMAP_ATTR_REF;
		if (flags & VM_PROT_WRITE)
			*attr |= PMAP_ATTR_CHG;
	}

	s = splvm();

	/* Insert page into page table. */
	if (__predict_false(pte_enter(pm, va, tte))) {
		if (__predict_false((flags & PMAP_CANFAIL) == 0))
			panic("%s: pte_enter", __func__);
		splx(s);
		return ENOMEM;
	}

	/* If this is a real fault, enter it in the tlb */
	if (tte && ((flags & PMAP_WIRED) == 0)) {
		int s2 = splhigh();
		ppc4xx_tlb_enter(pm->pm_ctx, va, tte);
		splx(s2);
	}

	splx(s);

	/* Flush the real memory from the instruction cache. */
	if ((prot & VM_PROT_EXECUTE) && (tte & TTE_I) == 0)
		__syncicache((void *)pa, PAGE_SIZE);

	return 0;
}

void
pmap_unwire(struct pmap *pm, vaddr_t va)
{
	struct pv_entry *pv;
	paddr_t pa;
	int s;

	if (!pmap_extract(pm, va, &pa))
		return;

	pv = pa_to_pv(pa);
	if (!pv)
		return;

	s = splvm();

	while (pv != NULL) {
		if (pm == pv->pv_pm && PV_CMPVA(va, pv)) {
			if (PV_ISWIRED(pv)) {
				PV_UNWIRE(pv);
				pm->pm_stats.wired_count--;
			}
			break;
		}
		pv = pv->pv_next;
	}

	splx(s);
}

void
pmap_kenter_pa(vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{
	struct pmap *pm = pmap_kernel();
	u_int tte;
	int s;

	/*
	 * Generate TTE.
	 *
	 * XXXX
	 *
	 * Since the kernel does not handle execution privileges properly,
	 * we will handle read and execute permissions together.
	 */
	tte = 0;
	if (prot & VM_PROT_ALL) {
		tte = TTE_PA(pa) | TTE_EX | TTE_ZONE(ZONE_PRIV);
		/* XXXX -- need to support multiple page sizes. */
		tte |= TTE_SZ_16K;

		KASSERT((flags & (PMAP_NOCACHE | PME_WRITETHROUG)) !=
		    (PMAP_NOCACHE | PME_WRITETHROUG));

		if (flags & PMAP_NOCACHE)
			/* Must be I/O mapping */
			tte |= TTE_I | TTE_G;
#ifdef PPC_4XX_NOCACHE
		tte |= TTE_I;
#else
		else if (prot & PME_WRITETHROUG) {
			/* Uncached and writethrough are not compatible */
			tte |= TTE_W;
		}
#endif
		if (prot & VM_PROT_WRITE)
			tte |= TTE_WR;
	}

	s = splvm();

	/* Insert page into page table. */
	if (__predict_false(pte_enter(pm, va, tte)))
		panic("%s: pte_enter", __func__);

	splx(s);
}

void
pmap_kremove(vaddr_t va, vsize_t len)
{

	while (len > 0) {
		(void)pte_enter(pmap_kernel(), va, 0);	/* never fail */
		va += PAGE_SIZE;
		len -= PAGE_SIZE;
	}
}

/*
 * Remove the given range of mapping entries.
 */
void
pmap_remove(struct pmap *pm, vaddr_t va, vaddr_t endva)
{
	paddr_t pa;
	volatile u_int *ptp;
	int s;

	s = splvm();

	while (va < endva) {
		if ((ptp = pte_find(pm, va)) && (pa = *ptp)) {
			pa = TTE_PA(pa);
			pmap_remove_pv(pm, va, pa);
			*ptp = 0;
			ppc4xx_tlb_flush(va, pm->pm_ctx);
			pm->pm_stats.resident_count--;
		}
		va += PAGE_SIZE;
	}

	splx(s);
}

/*
 * Get the physical page address for the given pmap/virtual address.
 */
bool
pmap_extract(struct pmap *pm, vaddr_t va, paddr_t *pap)
{
	int seg = STIDX(va), ptn = PTIDX(va);
	u_int pa = 0;
	int s;

	s = splvm();

	if (pm->pm_ptbl[seg] && (pa = pm->pm_ptbl[seg][ptn]) && pap)
		*pap = TTE_PA(pa) | (va & PGOFSET);

	splx(s);

	return pa != 0;
}

/*
 * Lower the protection on the specified range of this pmap.
 *
 * There are only two cases: either the protection is going to 0,
 * or it is going to read-only.
 */
void
pmap_protect(struct pmap *pm, vaddr_t sva, vaddr_t eva, vm_prot_t prot)
{
	volatile u_int *ptp;
	int s, bic;

	if ((prot & VM_PROT_READ) == 0) {
		pmap_remove(pm, sva, eva);
		return;
	}
	bic = 0;
	if ((prot & VM_PROT_WRITE) == 0)
		bic |= TTE_WR;
	if ((prot & VM_PROT_EXECUTE) == 0)
		bic |= TTE_EX;
	if (bic == 0)
		return;

	s = splvm();

	while (sva < eva) {
		if ((ptp = pte_find(pm, sva)) != NULL) {
			*ptp &= ~bic;
			ppc4xx_tlb_flush(sva, pm->pm_ctx);
		}
		sva += PAGE_SIZE;
	}

	splx(s);
}

bool
pmap_check_attr(struct vm_page *pg, u_int mask, int clear)
{
	paddr_t pa;
	char *attr;
	int s, rv;

	/*
	 * First modify bits in cache.
	 */
	pa = VM_PAGE_TO_PHYS(pg);
	attr = pa_to_attr(pa);
	if (attr == NULL)
		return false;

	s = splvm();

	rv = (*attr & mask) != 0;
	if (clear) {
		*attr &= ~mask;
		pmap_page_protect(pg,
		    mask == PMAP_ATTR_CHG ? VM_PROT_READ : 0);
	}

	splx(s);

	return rv;
}


/*
 * Lower the protection on the specified physical page.
 *
 * There are only two cases: either the protection is going to 0,
 * or it is going to read-only.
 */
void
pmap_page_protect(struct vm_page *pg, vm_prot_t prot)
{
	struct pv_entry *pvh, *pv, *npv;
	struct pmap *pm;
	paddr_t pa = VM_PAGE_TO_PHYS(pg);
	vaddr_t va;

	pvh = pa_to_pv(pa);
	if (pvh == NULL)
		return;

	/* Handle extra pvs which may be deleted in the operation */
	for (pv = pvh->pv_next; pv; pv = npv) {
		npv = pv->pv_next;

		pm = pv->pv_pm;
		va = PV_VA(pv);
		pmap_protect(pm, va, va + PAGE_SIZE, prot);
	}

	/* Now check the head pv */
	if (pvh->pv_pm) {
		pv = pvh;
		pm = pv->pv_pm;
		va = PV_VA(pv);
		pmap_protect(pm, va, va + PAGE_SIZE, prot);
	}
}

/*
 * Activate the address space for the specified process.  If the process
 * is the current process, load the new MMU context.
 */
void
pmap_activate(struct lwp *l)
{
#if 0
	struct pcb *pcb = lwp_getpcb(l);
	pmap_t pmap = l->l_proc->p_vmspace->vm_map.pmap;

	/*
	 * XXX Normally performed in cpu_lwp_fork().
	 */
	printf("pmap_activate(%p), pmap=%p\n",l,pmap);
	pcb->pcb_pm = pmap;
#endif
}

/*
 * Deactivate the specified process's address space.
 */
void
pmap_deactivate(struct lwp *l)
{
}

/*
 * Synchronize caches corresponding to [addr, addr+len) in p.
 */
void
pmap_procwr(struct proc *p, vaddr_t va, size_t len)
{
	struct pmap *pm = p->p_vmspace->vm_map.pmap;

	if (__predict_true(p == curproc)) {
		int msr, ctx, pid;

		/*
		 * Take it easy! TLB miss handler takes care of us.
		 */

		/*
	 	 * Need to turn off IMMU and switch to user context.
		 * (icbi uses DMMU).
		 */

		if (!(ctx = pm->pm_ctx)) {
			/* No context -- assign it one */
			ctx_alloc(pm);
			ctx = pm->pm_ctx;
		}

		__asm volatile (
			"mfmsr	%[msr];"
			"li	%[pid],0x20;"		/* Turn off IMMU */
			"andc	%[pid],%[msr],%[pid];"
			"ori	%[pid],%[pid],0x10;" /* Turn on DMMU for sure */
			"mtmsr	%[pid];"
			"isync;"
			MFPID(%[pid])
			MTPID(%[ctx])
			"isync;"
		"1:"
			"dcbst	0,%[va];"
			"icbi	0,%[va];"
			"add	%[va],%[va],%[size];"
			"sub.	%[len],%[len],%[size];"
			"bge	1b;"
			"sync;"
			MTPID(%[pid])
			"mtmsr	%[msr];"
			"isync;"
			: [msr] "=&r" (msr), [pid] "=&r" (pid)
			: [ctx] "r" (ctx), [va] "r" (va), [len] "r" (len),
			  [size] "r" (CACHELINESIZE));
	} else {
		paddr_t pa;
		vaddr_t tva, eva;
		int tlen;

		/*
		 * For p != curproc, we cannot rely upon TLB miss handler in
		 * user context. Therefore, extract pa and operate against it.
		 *
		 * Note that va below VM_MIN_KERNEL_ADDRESS is reserved for
		 * direct mapping.
		 */

		for (tva = va; len > 0; tva = eva, len -= tlen) {
			eva = uimin(tva + len, trunc_page(tva + PAGE_SIZE));
			tlen = eva - tva;
			if (!pmap_extract(pm, tva, &pa)) {
				/* XXX should be already unmapped */
				continue;
			}
			__syncicache((void *)pa, tlen);
		}
	}
}

static inline void
tlb_invalidate_entry(int i)
{
#ifdef PMAP_TLBDEBUG
	/*
	 * Clear only TLBHI[V] bit so that we can track invalidated entry.
	 */
	register_t msr, pid, hi;

	KASSERT(mfspr(SPR_PID) == KERNEL_PID);

	__asm volatile (
		"mfmsr	%[msr];"
		"li	%[pid],0;"
		"mtmsr	%[pid];"
		MFPID(%[pid])
		"tlbre	%[hi],%[i],0;"
		"andc	%[hi],%[hi],%[valid];"
		"tlbwe	%[hi],%[i],0;"
		MTPID(%[pid])
		"mtmsr	%[msr];"
		"isync;"
		: [msr] "=&r" (msr), [pid] "=&r" (pid), [hi] "=&r" (hi)
		: [i] "r" (i), [valid] "r" (TLB_VALID));
#else
	/*
	 * Just clear entire TLBHI register.
	 */
	__asm volatile (
		"tlbwe	%0,%1,0;"
		"isync;"
		: : "r" (0), "r" (i));
#endif

	tlb_info[i].ti_ctx = 0;
	tlb_info[i].ti_flags = 0;
}

/* This has to be done in real mode !!! */
void
ppc4xx_tlb_flush(vaddr_t va, int pid)
{
	u_long msr, i, found;

	/* If there's no context then it can't be mapped. */
	if (!pid)
		return;

	__asm volatile (
		MFPID(%[found])		/* Save PID */
		"mfmsr	%[msr];"	/* Save MSR */
		"li	%[i],0;"	/* Now clear MSR */
		"mtmsr	%[i];"
		"isync;"
		MTPID(%[pid])		/* Set PID */
		"isync;"
		"tlbsx.	%[i],0,%[va];"	/* Search TLB */
		"isync;"
		MTPID(%[found])		/* Restore PID */
		"mtmsr	%[msr];"	/* Restore MSR */
		"isync;"
		"li	%[found],1;"
		"beq	1f;"
		"li	%[found],0;"
	"1:"
		: [i] "=&r" (i), [found] "=&r" (found), [msr] "=&r" (msr)
		: [va] "r" (va), [pid] "r" (pid));

	if (found && !TLB_LOCKED(i)) {
		/* Now flush translation */
		tlb_invalidate_entry(i);
		tlbnext = i;
		/* Successful flushes */
		tlbflush_ev.ev_count++;
	}
}

void
ppc4xx_tlb_flush_all(void)
{
	u_long i;

	for (i = 0; i < NTLB; i++)
		if (!TLB_LOCKED(i))
			tlb_invalidate_entry(i);

	__asm volatile ("isync");
}

/* Find a TLB entry to evict. */
static int
ppc4xx_tlb_find_victim(void)
{
	int flags;

	for (;;) {
		if (++tlbnext >= NTLB)
			tlbnext = tlb_nreserved;
		flags = tlb_info[tlbnext].ti_flags;
		if (!(flags & TLBF_USED) ||
		    (flags & (TLBF_LOCKED | TLBF_REF)) == 0) {
			u_long va, stack = (u_long)&va;

			if (!((tlb_info[tlbnext].ti_va ^ stack) &
				(~PGOFSET)) &&
			    (tlb_info[tlbnext].ti_ctx == KERNEL_PID) &&
			    (flags & TLBF_USED)) {
				/* Kernel stack page */
				flags |= TLBF_REF;
				tlb_info[tlbnext].ti_flags = flags;
			} else {
				/* Found it! */
				return tlbnext;
			}
		} else
			tlb_info[tlbnext].ti_flags = (flags & ~TLBF_REF);
	}
}

void
ppc4xx_tlb_enter(int ctx, vaddr_t va, u_int pte)
{
	u_long hi, lo, i;
	paddr_t pa;
	int msr, pid, sz;

	tlbenter_ev.ev_count++;

	sz = (pte & TTE_SZ_MASK) >> TTE_SZ_SHIFT;
	pa = (pte & TTE_RPN_MASK(sz));
	hi = (va & TLB_EPN_MASK) | (sz << TLB_SIZE_SHFT) | TLB_VALID;
	lo = (pte & ~TLB_RPN_MASK) | pa;
	lo |= ppc4xx_tlbflags(va, pa);

	i = ppc4xx_tlb_find_victim();

	KASSERTMSG(i >= tlb_nreserved && i < NTLB,
	    "invalid entry %ld", i);

	tlb_info[i].ti_va = (va & TLB_EPN_MASK);
	tlb_info[i].ti_ctx = ctx;
	tlb_info[i].ti_flags = TLBF_USED | TLBF_REF;

	__asm volatile (
		"mfmsr	%[msr];"		/* Save MSR */
		"li	%[pid],0;"
		"mtmsr	%[pid];"		/* Clear MSR */
		"isync;"
		"tlbwe	%[pid],%[i],0;"		/* Invalidate old entry. */
		MFPID(%[pid])			/* Save old PID */
		MTPID(%[ctx])			/* Load translation ctx */
		"isync;"
		"tlbwe	%[lo],%[i],1;"		/* Set TLB */
		"tlbwe	%[hi],%[i],0;"
		"isync;"
		MTPID(%[pid])			/* Restore PID */
		"mtmsr	%[msr];"		/* and MSR */
		"isync;"
		: [msr] "=&r" (msr), [pid] "=&r" (pid)
		: [ctx] "r" (ctx), [i] "r" (i), [lo] "r" (lo), [hi] "r" (hi));
}

void
ppc4xx_tlb_init(void)
{
	int i;

	/* Mark reserved TLB entries */
	for (i = 0; i < tlb_nreserved; i++) {
		tlb_info[i].ti_flags = TLBF_LOCKED | TLBF_USED;
		tlb_info[i].ti_ctx = KERNEL_PID;
	}

	/* Setup security zones */
	/* Z0 - accessible by kernel only if TLB entry permissions allow
	 * Z1,Z2 - access is controlled by TLB entry permissions
	 * Z3 - full access regardless of TLB entry permissions
	 */

	__asm volatile (
		"mtspr	%0,%1;"
		"isync;"
		: : "K" (SPR_ZPR), "r" (0x1b000000));
}

/*
 * ppc4xx_tlb_size_mask:
 *
 * 	Roundup size to supported page size, return TLBHI mask and real size.
 */
static int
ppc4xx_tlb_size_mask(size_t size, int *mask, int *rsiz)
{
	int i;

	for (i = 0; i < __arraycount(tlbsize); i++)
		if (size <= tlbsize[i]) {
			*mask = (i << TLB_SIZE_SHFT);
			*rsiz = tlbsize[i];
			return 0;
		}
	return EINVAL;
}

/*
 * ppc4xx_tlb_mapiodev:
 *
 * 	Lookup virtual address of mapping previously entered via
 * 	ppc4xx_tlb_reserve. Search TLB directly so that we don't
 * 	need to waste extra storage for reserved mappings. Note
 * 	that reading TLBHI also sets PID, but all reserved mappings
 * 	use KERNEL_PID, so the side effect is nil.
 */
void *
ppc4xx_tlb_mapiodev(paddr_t base, psize_t len)
{
	paddr_t pa;
	vaddr_t va;
	u_int lo, hi, sz;
	int i;

	/* tlb_nreserved is only allowed to grow, so this is safe. */
	for (i = 0; i < tlb_nreserved; i++) {
		__asm volatile (
			"tlbre	%[lo],%[i],1;" 	/* TLBLO */
			"tlbre	%[hi],%[i],0;" 	/* TLBHI */
			: [lo] "=&r" (lo), [hi] "=&r" (hi)
			: [i] "r" (i));

		KASSERT(hi & TLB_VALID);
		KASSERT(mfspr(SPR_PID) == KERNEL_PID);

		pa = (lo & TLB_RPN_MASK);
		if (base < pa)
			continue;

		sz = tlbsize[(hi & TLB_SIZE_MASK) >> TLB_SIZE_SHFT];
		if (base + len > pa + sz)
			continue;

		va = (hi & TLB_EPN_MASK) + (base & (sz - 1)); 	/* sz = 2^n */
		return (void *)va;
	}

	return NULL;
}

/*
 * ppc4xx_tlb_reserve:
 *
 * 	Map physical range to kernel virtual chunk via reserved TLB entry.
 */
void
ppc4xx_tlb_reserve(paddr_t pa, vaddr_t va, size_t size, int flags)
{
	u_int lo, hi;
	int szmask, rsize;

	/* Called before pmap_bootstrap(), va outside kernel space. */
	KASSERT(va < VM_MIN_KERNEL_ADDRESS || va >= VM_MAX_KERNEL_ADDRESS);
	KASSERT(!pmap_bootstrap_done);
	KASSERT(tlb_nreserved < NTLB);

	/* Resolve size. */
	if (ppc4xx_tlb_size_mask(size, &szmask, &rsize) != 0)
		panic("ppc4xx_tlb_reserve: entry %d, %zuB too large",
		    size, tlb_nreserved);

	/* Real size will be power of two >= 1024, so this is OK. */
	pa &= ~(rsize - 1); 	/* RPN */
	va &= ~(rsize - 1); 	/* EPN */

	lo = pa | TLB_WR | flags;
	hi = va | TLB_VALID | szmask;

#ifdef PPC_4XX_NOCACHE
	lo |= TLB_I;
#endif

	__asm volatile (
		"tlbwe	%[lo],%[i],1;"	/* write TLBLO */
		"tlbwe	%[hi],%[i],0;"	/* write TLBHI */
		"isync;"
		: : [i] "r" (tlb_nreserved), [lo] "r" (lo), [hi] "r" (hi));

	tlb_nreserved++;
}

/*
 * We should pass the ctx in from trap code.
 */
int
pmap_tlbmiss(vaddr_t va, int ctx)
{
	volatile u_int *pte;
	u_long tte;

	tlbmiss_ev.ev_count++;

	/*
	 * We will reserve 0 upto VM_MIN_KERNEL_ADDRESS for va == pa mappings.
	 * Physical RAM is expected to live in this range, care must be taken
	 * to not clobber 0 upto ${physmem} with device mappings in machdep
	 * code.
	 */
	if (ctx != KERNEL_PID ||
	    (va >= VM_MIN_KERNEL_ADDRESS && va < VM_MAX_KERNEL_ADDRESS)) {
		pte = pte_find((struct pmap *)__UNVOLATILE(ctxbusy[ctx]), va);
		if (pte == NULL) {
			/*
			 * Map unmanaged addresses directly for
			 * kernel access
			 */
			return 1;
		}
		tte = *pte;
		if (tte == 0)
			return 1;
	} else {
		/* Create a 16MB writable mapping. */
		tte = TTE_PA(va) | TTE_ZONE(ZONE_PRIV) | TTE_SZ_16M | TTE_WR;
#ifdef PPC_4XX_NOCACHE
		tte |= TTE_I;
#endif
	}
	ppc4xx_tlb_enter(ctx, va, tte);

	return 0;
}

/*
 * Flush all the entries matching a context from the TLB.
 */
static void
ctx_flush(int cnum)
{
	int i;

	/* We gotta steal this context */
	for (i = tlb_nreserved; i < NTLB; i++) {
		if (tlb_info[i].ti_ctx == cnum) {
			/* Can't steal ctx if it has locked/reserved entry. */
			KASSERTMSG(!TLB_LOCKED(i) && i >= tlb_nreserved,
			    "locked/reserved entry %d for ctx %d",
			    i, cnum);
			/*
			 * Invalidate particular TLB entry regardless of
			 * locked status
			 */
			tlb_invalidate_entry(i);
		}
	}
}

/*
 * Allocate a context.  If necessary, steal one from someone else.
 *
 * The new context is flushed from the TLB before returning.
 */
int
ctx_alloc(struct pmap *pm)
{
	static int next = MINCTX;
	int cnum, s;

	KASSERT(pm != pmap_kernel());

	s = splvm();

	/* Find a likely context. */
	cnum = next;
	do {
		if (++cnum >= NUMCTX)
			cnum = MINCTX;
	} while (ctxbusy[cnum] != NULL && cnum != next);

	/* Now clean it out */
	if (cnum < MINCTX)
		cnum = MINCTX; /* Never steal ctx 0 or 1 */
	ctx_flush(cnum);

	if (ctxbusy[cnum]) {
#ifdef DEBUG
		/* We should identify this pmap and clear it */
		printf("Warning: stealing context %d\n", cnum);
#endif
		ctxbusy[cnum]->pm_ctx = 0;
	}
	ctxbusy[cnum] = pm;
	next = cnum;

	splx(s);

	pm->pm_ctx = cnum;

	return cnum;
}

/*
 * Give away a context.
 */
void
ctx_free(struct pmap *pm)
{
	int oldctx;

	oldctx = pm->pm_ctx;

	if (oldctx == 0)
		panic("ctx_free: freeing kernel context");

	KASSERTMSG(ctxbusy[oldctx] == pm,
	    "ctxbusy[%d] = %p, pm->pm_ctx = %p",
	    oldctx, ctxbusy[oldctx], pm);

	/* We should verify it has not been stolen and reallocated... */
	ctxbusy[oldctx] = NULL;
	ctx_flush(oldctx);
}

#ifdef DEBUG
/*
 * Test ref/modify handling.
 */
void pmap_testout(void);
void
pmap_testout(void)
{
	struct vm_page *pg;
	vaddr_t va;
	paddr_t pa;
	volatile int *loc;
	int ref, mod, val = 0;

	/* Allocate a page */
	va = (vaddr_t)uvm_km_alloc(kernel_map, PAGE_SIZE, 0,
	    UVM_KMF_WIRED | UVM_KMF_ZERO);
	loc = (int *)va;

	pmap_extract(pmap_kernel(), va, &pa);
	pg = PHYS_TO_VM_PAGE(pa);
	pmap_unwire(pmap_kernel(), va);

	pmap_kremove(va, PAGE_SIZE);
	pmap_enter(pmap_kernel(), va, pa, VM_PROT_ALL, 0);
	pmap_update(pmap_kernel());

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Check it's properly cleared */
	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Checking cleared page: ref %d, mod %d\n", ref, mod);

	/* Reference page */
	val = *loc;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Referenced page: ref %d, mod %d val %x\n", ref, mod, val);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Modify page */
	*loc = 1;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Modified page: ref %d, mod %d\n", ref, mod);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Check it's properly cleared */
	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Checking cleared page: ref %d, mod %d\n", ref, mod);

	/* Modify page */
	*loc = 1;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Modified page: ref %d, mod %d\n", ref, mod);

	/* Check pmap_protect() */
	pmap_protect(pmap_kernel(), va, va + PAGE_SIZE, VM_PROT_READ);
	pmap_update(pmap_kernel());
	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("pmap_protect(VM_PROT_READ): ref %d, mod %d\n", ref, mod);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Reference page */
	val = *loc;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Referenced page: ref %d, mod %d val %x\n", ref, mod, val);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Modify page */
#if 0
	pmap_enter(pmap_kernel(), va, pa, VM_PROT_ALL, 0);
	pmap_update(pmap_kernel());
#endif
	*loc = 1;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Modified page: ref %d, mod %d\n", ref, mod);

	/* Check pmap_protect() */
	pmap_protect(pmap_kernel(), va, va + PAGE_SIZE, VM_PROT_NONE);
	pmap_update(pmap_kernel());
	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("pmap_protect(): ref %d, mod %d\n", ref, mod);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Reference page */
	val = *loc;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Referenced page: ref %d, mod %d val %x\n", ref, mod, val);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Modify page */
#if 0
	pmap_enter(pmap_kernel(), va, pa, VM_PROT_ALL, 0);
	pmap_update(pmap_kernel());
#endif
	*loc = 1;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Modified page: ref %d, mod %d\n", ref, mod);

	/* Check pmap_pag_protect() */
	pmap_page_protect(pg, VM_PROT_READ);
	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("pmap_page_protect(VM_PROT_READ): ref %d, mod %d\n", ref, mod);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Reference page */
	val = *loc;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Referenced page: ref %d, mod %d val %x\n", ref, mod, val);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Modify page */
#if 0
	pmap_enter(pmap_kernel(), va, pa, VM_PROT_ALL, 0);
	pmap_update(pmap_kernel());
#endif
	*loc = 1;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Modified page: ref %d, mod %d\n", ref, mod);

	/* Check pmap_pag_protect() */
	pmap_page_protect(pg, VM_PROT_NONE);
	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("pmap_page_protect(): ref %d, mod %d\n", ref, mod);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);


	/* Reference page */
	val = *loc;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Referenced page: ref %d, mod %d val %x\n", ref, mod, val);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Modify page */
#if 0
	pmap_enter(pmap_kernel(), va, pa, VM_PROT_ALL, 0);
	pmap_update(pmap_kernel());
#endif
	*loc = 1;

	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Modified page: ref %d, mod %d\n", ref, mod);

	/* Unmap page */
	pmap_remove(pmap_kernel(), va, va + PAGE_SIZE);
	pmap_update(pmap_kernel());
	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Unmapped page: ref %d, mod %d\n", ref, mod);

	/* Now clear reference and modify */
	ref = pmap_clear_reference(pg);
	mod = pmap_clear_modify(pg);
	printf("Clearing page va %p pa %lx: ref %d, mod %d\n",
	    (void *)(u_long)va, (long)pa, ref, mod);

	/* Check it's properly cleared */
	ref = pmap_is_referenced(pg);
	mod = pmap_is_modified(pg);
	printf("Checking cleared page: ref %d, mod %d\n", ref, mod);

	pmap_remove(pmap_kernel(), va, va + PAGE_SIZE);
	pmap_kenter_pa(va, pa, VM_PROT_ALL, 0);
	uvm_km_free(kernel_map, (vaddr_t)va, PAGE_SIZE, UVM_KMF_WIRED);
}
#endif
