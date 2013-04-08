#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>

#include "crtools.h"
#include "mem.h"
#include "parasite-syscall.h"
#include "parasite.h"
#include "page-pipe.h"
#include "page-xfer.h"
#include "log.h"

#define PME_PRESENT	(1ULL << 63)
#define PME_SWAP	(1ULL << 62)
#define PME_FILE	(1ULL << 61)

unsigned int vmas_pagemap_size(struct vm_area_list *vmas)
{
	/*
	 * In the worst case I need one iovec for half of the
	 * pages (e.g. every odd/even)
	 */

	return sizeof(struct parasite_dump_pages_args) +
		(vmas->priv_size + 1) * sizeof(struct iovec) / 2;
}

static inline bool should_dump_page(VmaEntry *vmae, u64 pme)
{
	if (vma_entry_is(vmae, VMA_AREA_VDSO))
		return true;
	/*
	 * Optimisation for private mapping pages, that haven't
	 * yet being COW-ed
	 */
	if (vma_entry_is(vmae, VMA_FILE_PRIVATE) && (pme & PME_FILE))
		return false;
	if (pme & (PME_PRESENT | PME_SWAP))
		return true;

	return false;
}

static int generate_iovs(struct vma_area *vma, int pagemap, struct page_pipe *pp, u64 *map)
{
	unsigned long pfn, nr_to_scan;
	u64 aux;

	aux = vma->vma.start / PAGE_SIZE * sizeof(*map);
	if (lseek(pagemap, aux, SEEK_SET) != aux) {
		pr_perror("Can't rewind pagemap file");
		return -1;
	}

	nr_to_scan = vma_area_len(vma) / PAGE_SIZE;
	aux = nr_to_scan * sizeof(*map);
	if (read(pagemap, map, aux) != aux) {
		pr_perror("Can't read pagemap file");
		return -1;
	}

	for (pfn = 0; pfn < nr_to_scan; pfn++) {
		if (!should_dump_page(&vma->vma, map[pfn]))
			continue;

		if (page_pipe_add_page(pp, vma->vma.start + pfn * PAGE_SIZE))
			return -1;
	}

	return 0;
}

static int parasite_mprotect_seized(struct parasite_ctl *ctl, struct vm_area_list *vma_area_list, bool unprotect)
{
	struct parasite_mprotect_args *args;
	struct parasite_vma_entry *p_vma;
	struct vma_area *vma;

	args = parasite_args_s(ctl, vmas_pagemap_size(vma_area_list));

	p_vma = args->vmas;
	args->nr = 0;

	list_for_each_entry(vma, &vma_area_list->h, list) {
		if (!privately_dump_vma(vma))
			continue;
		if (vma->vma.prot & PROT_READ)
			continue;
		p_vma->start = vma->vma.start;
		p_vma->len = vma_area_len(vma);
		p_vma->prot = vma->vma.prot;
		if (unprotect)
			p_vma->prot |= PROT_READ;
		args->nr++;
		p_vma++;
	}

	return parasite_execute(PARASITE_CMD_MPROTECT_VMAS, ctl);
}

static int __parasite_dump_pages_seized(struct parasite_ctl *ctl, int vpid,
		struct vm_area_list *vma_area_list, struct cr_fdset *cr_fdset)
{
	struct parasite_dump_pages_args *args;
	u64 *map;
	int pagemap;
	struct page_pipe *pp;
	struct page_pipe_buf *ppb;
	struct vma_area *vma_area;
	int ret = -1;
	struct page_xfer xfer;

	pr_info("\n");
	pr_info("Dumping pages (type: %d pid: %d)\n", CR_FD_PAGES, ctl->pid.real);
	pr_info("----------------------------------------\n");

	pr_debug("   Private vmas %lu/%lu pages\n",
			vma_area_list->longest, vma_area_list->priv_size);

	args = parasite_args_s(ctl, vmas_pagemap_size(vma_area_list));

	map = xmalloc(vma_area_list->longest * sizeof(*map));
	if (!map)
		goto out;

	ret = pagemap = open_proc(ctl->pid.real, "pagemap");
	if (ret < 0)
		goto out_free;

	ret = -1;
	pp = create_page_pipe(vma_area_list->priv_size / 2, args->iovs);
	if (!pp)
		goto out_close;

	list_for_each_entry(vma_area, &vma_area_list->h, list) {
		if (!privately_dump_vma(vma_area))
			continue;

		ret = generate_iovs(vma_area, pagemap, pp, map);
		if (ret < 0)
			goto out_pp;
	}

	args->off = 0;
	list_for_each_entry(ppb, &pp->bufs, l) {
		ret = parasite_send_fd(ctl, ppb->p[1]);
		if (ret)
			goto out_pp;

		args->nr = ppb->nr_segs;
		args->nr_pages = ppb->pages_in;
		pr_debug("PPB: %d pages %d segs %u pipe %d off\n",
				args->nr_pages, args->nr, ppb->pipe_size, args->off);

		ret = parasite_execute(PARASITE_CMD_DUMPPAGES, ctl);
		if (ret < 0)
			goto out_pp;

		args->off += args->nr;
	}

	ret = open_page_xfer(&xfer, CR_FD_PAGEMAP, vpid);
	if (ret < 0)
		goto out_pp;

	ret = page_xfer_dump_pages(&xfer, pp, 0);

	xfer.close(&xfer);
out_pp:
	destroy_page_pipe(pp);
out_close:
	close(pagemap);
out_free:
	xfree(map);
out:
	pr_info("----------------------------------------\n");
	return ret;
}

int parasite_dump_pages_seized(struct parasite_ctl *ctl, int vpid,
		struct vm_area_list *vma_area_list, struct cr_fdset *cr_fdset)
{
	int ret;

	ret = parasite_mprotect_seized(ctl, vma_area_list, true);
	if (ret) {
		pr_err("Can't dump unprotect vmas with parasite\n");
		return ret;
	}

	ret = __parasite_dump_pages_seized(ctl, vpid, vma_area_list, cr_fdset);
	if (ret)
		pr_err("Can't dump page with parasite\n");

	if (parasite_mprotect_seized(ctl, vma_area_list, false)) {
		pr_err("Can't rollback unprotected vmas with parasite\n");
		ret = -1;
	}

	return ret;
}

