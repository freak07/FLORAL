// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
 */

#include <linux/dma-contiguous.h>
#include <linux/dma-mapping.h>
#include <linux/dma-mapping-fast.h>
#include <linux/io-pgtable-fast.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <asm/dma-iommu.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/pci.h>
#include <trace/events/iommu.h>
#include "io-pgtable.h"

#include <soc/qcom/secure_buffer.h>
#include <linux/arm-smmu-errata.h>

/* some redundant definitions... :( TODO: move to io-pgtable-fast.h */
#define FAST_PAGE_SHIFT		12
#define FAST_PAGE_SIZE (1UL << FAST_PAGE_SHIFT)
#define FAST_PAGE_MASK (~(PAGE_SIZE - 1))

static pgprot_t __get_dma_pgprot(unsigned long attrs, pgprot_t prot,
				 bool coherent)
{
	if (attrs & DMA_ATTR_STRONGLY_ORDERED)
		return pgprot_noncached(prot);
	else if (!coherent || (attrs & DMA_ATTR_WRITE_COMBINE))
		return pgprot_writecombine(prot);
	return prot;
}

static int __get_iommu_pgprot(unsigned long attrs, int prot,
			      bool coherent)
{
	if (!(attrs & DMA_ATTR_EXEC_MAPPING))
		prot |= IOMMU_NOEXEC;
	if ((attrs & DMA_ATTR_STRONGLY_ORDERED))
		prot |= IOMMU_MMIO;
	if (coherent)
		prot |= IOMMU_CACHE;

	return prot;
}

static bool is_dma_coherent(struct device *dev, unsigned long attrs)
{
	bool is_coherent;

	if (attrs & DMA_ATTR_FORCE_COHERENT)
		is_coherent = true;
	else if (attrs & DMA_ATTR_FORCE_NON_COHERENT)
		is_coherent = false;
	else if (is_device_dma_coherent(dev))
		is_coherent = true;
	else
		is_coherent = false;

	return is_coherent;
}

static dma_addr_t __fast_smmu_alloc_iova(struct dma_fast_smmu_mapping *mapping,
					 unsigned long attrs,
					 size_t size)
{
	unsigned long bit, nbits;
	unsigned long align;
	unsigned long guard_len;
	dma_addr_t iova;

	if (mapping->min_iova_align)
		guard_len = ALIGN(size, mapping->min_iova_align) - size;
	else
		guard_len = 0;

	nbits = (size + guard_len) >> FAST_PAGE_SHIFT;
	align = (1 << get_order(size + guard_len)) - 1;
	bit = bitmap_find_next_zero_area(mapping->clean_bitmap,
					  mapping->num_4k_pages,
					  mapping->next_start, nbits, align);
	if (unlikely(bit > mapping->num_4k_pages)) {
		/* try wrapping */
		bit = bitmap_find_next_zero_area(
			mapping->clean_bitmap, mapping->num_4k_pages, 0, nbits,
			align);
		if (unlikely(bit > mapping->num_4k_pages)) {
			/*
			 * If we just re-allocated a VA whose TLB hasn't been
			 * invalidated since it was last used and unmapped, we
			 * need to invalidate it here.  We actually invalidate
			 * the entire TLB so that we don't have to invalidate
			 * the TLB again until we wrap back around.
			 */
			if (mapping->have_stale_tlbs) {
				bool skip_sync = (attrs &
						  DMA_ATTR_SKIP_CPU_SYNC);
				struct iommu_domain_geometry *geometry =
					&(mapping->domain->geometry);

				iommu_tlbiall(mapping->domain);
				bitmap_copy(mapping->clean_bitmap,
					    mapping->bitmap,
					    mapping->num_4k_pages);
				mapping->have_stale_tlbs = false;
				av8l_fast_clear_stale_ptes(mapping->pgtbl_ops,
						geometry->aperture_start,
						mapping->base,
						mapping->base +
						mapping->size - 1,
						skip_sync);
				bit = bitmap_find_next_zero_area(
							mapping->clean_bitmap,
							mapping->num_4k_pages,
								 0, nbits,
								 align);
				if (unlikely(bit > mapping->num_4k_pages))
					return DMA_ERROR_CODE;

			} else {
				return DMA_ERROR_CODE;
			}
		}
	}

	bitmap_set(mapping->bitmap, bit, nbits);
	bitmap_set(mapping->clean_bitmap, bit, nbits);
	mapping->next_start = bit + nbits;
	if (unlikely(mapping->next_start >= mapping->num_4k_pages))
		mapping->next_start = 0;

	iova =  (bit << FAST_PAGE_SHIFT) + mapping->base;
	if (guard_len &&
		iommu_map(mapping->domain, iova + size,
			page_to_phys(mapping->guard_page),
			guard_len, ARM_SMMU_GUARD_PROT)) {

		bitmap_clear(mapping->bitmap, bit, nbits);
		return DMA_ERROR_CODE;
	}
	return iova;
}

static void __fast_smmu_free_iova(struct dma_fast_smmu_mapping *mapping,
				  dma_addr_t iova, size_t size)
{
	unsigned long start_bit = (iova - mapping->base) >> FAST_PAGE_SHIFT;
	unsigned long nbits;
	unsigned long guard_len;

	if (mapping->min_iova_align) {
		guard_len = ALIGN(size, mapping->min_iova_align) - size;
		iommu_unmap(mapping->domain, iova + size, guard_len);
	} else {
		guard_len = 0;
	}
	nbits = (size + guard_len) >> FAST_PAGE_SHIFT;


	/*
	 * We don't invalidate TLBs on unmap.  We invalidate TLBs on map
	 * when we're about to re-allocate a VA that was previously
	 * unmapped but hasn't yet been invalidated.
	 */
	bitmap_clear(mapping->bitmap, start_bit, nbits);
	mapping->have_stale_tlbs = true;
}


static void __fast_dma_page_cpu_to_dev(struct page *page, unsigned long off,
				       size_t size, enum dma_data_direction dir)
{
	__dma_map_area(page_address(page) + off, size, dir);
}

static void __fast_dma_page_dev_to_cpu(struct page *page, unsigned long off,
				       size_t size, enum dma_data_direction dir)
{
	__dma_unmap_area(page_address(page) + off, size, dir);

	/* TODO: WHAT IS THIS? */
	/*
	 * Mark the D-cache clean for this page to avoid extra flushing.
	 */
	if (dir != DMA_TO_DEVICE && off == 0 && size >= PAGE_SIZE)
		set_bit(PG_dcache_clean, &page->flags);
}

static int __fast_dma_direction_to_prot(enum dma_data_direction dir)
{
	switch (dir) {
	case DMA_BIDIRECTIONAL:
		return IOMMU_READ | IOMMU_WRITE;
	case DMA_TO_DEVICE:
		return IOMMU_READ;
	case DMA_FROM_DEVICE:
		return IOMMU_WRITE;
	default:
		return 0;
	}
}

static dma_addr_t fast_smmu_map_page(struct device *dev, struct page *page,
				   unsigned long offset, size_t size,
				   enum dma_data_direction dir,
				   unsigned long attrs)
{
	struct dma_fast_smmu_mapping *mapping = dev->archdata.mapping->fast;
	dma_addr_t iova;
	unsigned long flags;
	phys_addr_t phys_plus_off = page_to_phys(page) + offset;
	phys_addr_t phys_to_map = round_down(phys_plus_off, FAST_PAGE_SIZE);
	unsigned long offset_from_phys_to_map = phys_plus_off & ~FAST_PAGE_MASK;
	size_t len = ALIGN(size + offset_from_phys_to_map, FAST_PAGE_SIZE);
	bool skip_sync = (attrs & DMA_ATTR_SKIP_CPU_SYNC);
	int prot = __fast_dma_direction_to_prot(dir);
	bool is_coherent = is_dma_coherent(dev, attrs);

	prot = __get_iommu_pgprot(attrs, prot, is_coherent);

	if (!skip_sync && !is_coherent)
		__fast_dma_page_cpu_to_dev(phys_to_page(phys_to_map),
					   offset_from_phys_to_map, size, dir);

	spin_lock_irqsave(&mapping->lock, flags);

	iova = __fast_smmu_alloc_iova(mapping, attrs, len);

	if (unlikely(iova == DMA_ERROR_CODE))
		goto fail;

	if (unlikely(av8l_fast_map_public(mapping->pgtbl_ops, iova,
					  phys_to_map, len, prot)))
		goto fail_free_iova;

	spin_unlock_irqrestore(&mapping->lock, flags);

	trace_map(mapping->domain, iova, phys_to_map, len, prot);
	return iova + offset_from_phys_to_map;

fail_free_iova:
	__fast_smmu_free_iova(mapping, iova, size);
fail:
	spin_unlock_irqrestore(&mapping->lock, flags);
	return DMA_ERROR_CODE;
}

static void fast_smmu_unmap_page(struct device *dev, dma_addr_t iova,
			       size_t size, enum dma_data_direction dir,
			       unsigned long attrs)
{
	struct dma_fast_smmu_mapping *mapping = dev->archdata.mapping->fast;
	unsigned long flags;
	unsigned long offset = iova & ~FAST_PAGE_MASK;
	size_t len = ALIGN(size + offset, FAST_PAGE_SIZE);
	bool skip_sync = (attrs & DMA_ATTR_SKIP_CPU_SYNC);
	bool is_coherent = is_dma_coherent(dev, attrs);

	if (!skip_sync && !is_coherent) {
		phys_addr_t phys;

		phys = av8l_fast_iova_to_phys_public(mapping->pgtbl_ops, iova);
		WARN_ON(!phys);

		__fast_dma_page_dev_to_cpu(phys_to_page(phys), offset,
						size, dir);
	}

	spin_lock_irqsave(&mapping->lock, flags);
	av8l_fast_unmap_public(mapping->pgtbl_ops, iova, len);
	__fast_smmu_free_iova(mapping, iova - offset, len);
	spin_unlock_irqrestore(&mapping->lock, flags);

	trace_unmap(mapping->domain, iova - offset, len, len);
}

static void fast_smmu_sync_single_for_cpu(struct device *dev,
		dma_addr_t iova, size_t size, enum dma_data_direction dir)
{
	struct dma_fast_smmu_mapping *mapping = dev->archdata.mapping->fast;
	unsigned long offset = iova & ~FAST_PAGE_MASK;

	if (!av8l_fast_iova_coherent_public(mapping->pgtbl_ops, iova)) {
		phys_addr_t phys;

		phys = av8l_fast_iova_to_phys_public(mapping->pgtbl_ops, iova);
		WARN_ON(!phys);

		__fast_dma_page_dev_to_cpu(phys_to_page(phys), offset,
						size, dir);
	}
}

static void fast_smmu_sync_single_for_device(struct device *dev,
		dma_addr_t iova, size_t size, enum dma_data_direction dir)
{
	struct dma_fast_smmu_mapping *mapping = dev->archdata.mapping->fast;
	unsigned long offset = iova & ~FAST_PAGE_MASK;

	if (!av8l_fast_iova_coherent_public(mapping->pgtbl_ops, iova)) {
		phys_addr_t phys;

		phys = av8l_fast_iova_to_phys_public(mapping->pgtbl_ops, iova);
		WARN_ON(!phys);

		__fast_dma_page_cpu_to_dev(phys_to_page(phys), offset,
						size, dir);
	}
}

static int fast_smmu_map_sg(struct device *dev, struct scatterlist *sg,
			    int nents, enum dma_data_direction dir,
			    unsigned long attrs)
{
	/* 0 indicates error */
	return 0;
}

static void fast_smmu_unmap_sg(struct device *dev,
			       struct scatterlist *sg, int nents,
			       enum dma_data_direction dir,
			       unsigned long attrs)
{
	WARN_ON_ONCE(1);
}

static void fast_smmu_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sg, int nents, enum dma_data_direction dir)
{
	WARN_ON_ONCE(1);
}

static void fast_smmu_sync_sg_for_device(struct device *dev,
		struct scatterlist *sg, int nents, enum dma_data_direction dir)
{
	WARN_ON_ONCE(1);
}

static void __fast_smmu_free_pages(struct page **pages, int count)
{
	int i;

	for (i = 0; i < count; i++)
		__free_page(pages[i]);
	kvfree(pages);
}

static struct page **__fast_smmu_alloc_pages(unsigned int count, gfp_t gfp)
{
	struct page **pages;
	unsigned int i = 0, array_size = count * sizeof(*pages);

	if (array_size <= PAGE_SIZE)
		pages = kzalloc(array_size, GFP_KERNEL);
	else
		pages = vzalloc(array_size);
	if (!pages)
		return NULL;

	/* IOMMU can map any pages, so himem can also be used here */
	gfp |= __GFP_NOWARN | __GFP_HIGHMEM;

	for (i = 0; i < count; ++i) {
		struct page *page = alloc_page(gfp);

		if (!page) {
			__fast_smmu_free_pages(pages, i);
			return NULL;
		}
		pages[i] = page;
	}
	return pages;
}

static void *fast_smmu_alloc(struct device *dev, size_t size,
			     dma_addr_t *handle, gfp_t gfp,
			     unsigned long attrs)
{
	struct dma_fast_smmu_mapping *mapping = dev->archdata.mapping->fast;
	struct sg_table sgt;
	dma_addr_t dma_addr, iova_iter;
	void *addr;
	unsigned long flags;
	struct sg_mapping_iter miter;
	size_t count = ALIGN(size, SZ_4K) >> PAGE_SHIFT;
	int prot = IOMMU_READ | IOMMU_WRITE; /* TODO: extract from attrs */
	bool is_coherent = is_dma_coherent(dev, attrs);
	pgprot_t remap_prot = __get_dma_pgprot(attrs, PAGE_KERNEL, is_coherent);
	struct page **pages;

	/*
	 * sg_alloc_table_from_pages accepts unsigned int value for count
	 * so check count doesn't exceed UINT_MAX.
	 */

	if (count > UINT_MAX) {
		dev_err(dev, "count: %zx exceeds UNIT_MAX\n", count);
		return NULL;
	}

	prot = __get_iommu_pgprot(attrs, prot, is_coherent);

	*handle = DMA_ERROR_CODE;

	pages = __fast_smmu_alloc_pages(count, gfp);
	if (!pages) {
		dev_err(dev, "no pages\n");
		return NULL;
	}

	size = ALIGN(size, SZ_4K);
	if (sg_alloc_table_from_pages(&sgt, pages, count, 0, size, gfp)) {
		dev_err(dev, "no sg tablen\n");
		goto out_free_pages;
	}

	if (!is_coherent) {
		/*
		 * The CPU-centric flushing implied by SG_MITER_TO_SG isn't
		 * sufficient here, so skip it by using the "wrong" direction.
		 */
		sg_miter_start(&miter, sgt.sgl, sgt.orig_nents,
			       SG_MITER_FROM_SG);
		while (sg_miter_next(&miter))
			__dma_flush_area(miter.addr, miter.length);
		sg_miter_stop(&miter);
	}

	spin_lock_irqsave(&mapping->lock, flags);
	dma_addr = __fast_smmu_alloc_iova(mapping, attrs, size);
	if (dma_addr == DMA_ERROR_CODE) {
		dev_err(dev, "no iova\n");
		spin_unlock_irqrestore(&mapping->lock, flags);
		goto out_free_sg;
	}
	iova_iter = dma_addr;
	sg_miter_start(&miter, sgt.sgl, sgt.orig_nents,
		       SG_MITER_FROM_SG | SG_MITER_ATOMIC);
	while (sg_miter_next(&miter)) {
		if (unlikely(av8l_fast_map_public(
				     mapping->pgtbl_ops, iova_iter,
				     page_to_phys(miter.page),
				     miter.length, prot))) {
			dev_err(dev, "no map public\n");
			/* TODO: unwind previously successful mappings */
			goto out_free_iova;
		}
		iova_iter += miter.length;
	}
	sg_miter_stop(&miter);
	spin_unlock_irqrestore(&mapping->lock, flags);

	addr = dma_common_pages_remap(pages, size, VM_USERMAP, remap_prot,
				      __builtin_return_address(0));
	if (!addr) {
		dev_err(dev, "no common pages\n");
		goto out_unmap;
	}

	*handle = dma_addr;
	sg_free_table(&sgt);
	return addr;

out_unmap:
	/* need to take the lock again for page tables and iova */
	spin_lock_irqsave(&mapping->lock, flags);
	av8l_fast_unmap_public(mapping->pgtbl_ops, dma_addr, size);
out_free_iova:
	__fast_smmu_free_iova(mapping, dma_addr, size);
	spin_unlock_irqrestore(&mapping->lock, flags);
out_free_sg:
	sg_free_table(&sgt);
out_free_pages:
	__fast_smmu_free_pages(pages, count);
	return NULL;
}

static void fast_smmu_free(struct device *dev, size_t size,
			   void *vaddr, dma_addr_t dma_handle,
			   unsigned long attrs)
{
	struct dma_fast_smmu_mapping *mapping = dev->archdata.mapping->fast;
	struct vm_struct *area;
	struct page **pages;
	size_t count = ALIGN(size, SZ_4K) >> FAST_PAGE_SHIFT;
	unsigned long flags;

	size = ALIGN(size, SZ_4K);

	area = find_vm_area(vaddr);
	if (WARN_ON_ONCE(!area))
		return;

	pages = area->pages;
	dma_common_free_remap(vaddr, size, VM_USERMAP, false);
	spin_lock_irqsave(&mapping->lock, flags);
	av8l_fast_unmap_public(mapping->pgtbl_ops, dma_handle, size);
	__fast_smmu_free_iova(mapping, dma_handle, size);
	spin_unlock_irqrestore(&mapping->lock, flags);
	__fast_smmu_free_pages(pages, count);
}

static int fast_smmu_mmap_attrs(struct device *dev, struct vm_area_struct *vma,
				void *cpu_addr, dma_addr_t dma_addr,
				size_t size, unsigned long attrs)
{
	struct vm_struct *area;
	unsigned long uaddr = vma->vm_start;
	struct page **pages;
	int i, nr_pages, ret = 0;
	bool coherent = is_dma_coherent(dev, attrs);

	vma->vm_page_prot = __get_dma_pgprot(attrs, vma->vm_page_prot,
					     coherent);
	area = find_vm_area(cpu_addr);
	if (!area)
		return -EINVAL;

	pages = area->pages;
	nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	for (i = vma->vm_pgoff; i < nr_pages && uaddr < vma->vm_end; i++) {
		ret = vm_insert_page(vma, uaddr, pages[i]);
		if (ret)
			break;
		uaddr += PAGE_SIZE;
	}

	return ret;
}

static int fast_smmu_get_sgtable(struct device *dev, struct sg_table *sgt,
				void *cpu_addr, dma_addr_t dma_addr,
				size_t size, unsigned long attrs)
{
	unsigned int n_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct vm_struct *area;

	area = find_vm_area(cpu_addr);
	if (!area || !area->pages)
		return -EINVAL;

	return sg_alloc_table_from_pages(sgt, area->pages, n_pages, 0, size,
					GFP_KERNEL);
}

static dma_addr_t fast_smmu_dma_map_resource(
			struct device *dev, phys_addr_t phys_addr,
			size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	struct dma_fast_smmu_mapping *mapping = dev->archdata.mapping->fast;
	size_t offset = phys_addr & ~FAST_PAGE_MASK;
	size_t len = round_up(size + offset, FAST_PAGE_SIZE);
	dma_addr_t dma_addr;
	int prot;
	unsigned long flags;

	spin_lock_irqsave(&mapping->lock, flags);
	dma_addr = __fast_smmu_alloc_iova(mapping, attrs, len);
	spin_unlock_irqrestore(&mapping->lock, flags);

	if (dma_addr == DMA_ERROR_CODE)
		return dma_addr;

	prot = __fast_dma_direction_to_prot(dir);
	prot |= IOMMU_MMIO;

	if (iommu_map(mapping->domain, dma_addr, phys_addr - offset,
			len, prot)) {
		spin_lock_irqsave(&mapping->lock, flags);
		__fast_smmu_free_iova(mapping, dma_addr, len);
		spin_unlock_irqrestore(&mapping->lock, flags);
		return DMA_ERROR_CODE;
	}
	return dma_addr + offset;
}

static void fast_smmu_dma_unmap_resource(
			struct device *dev, dma_addr_t addr,
			size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	struct dma_fast_smmu_mapping *mapping = dev->archdata.mapping->fast;
	size_t offset = addr & ~FAST_PAGE_MASK;
	size_t len = round_up(size + offset, FAST_PAGE_SIZE);
	unsigned long flags;

	iommu_unmap(mapping->domain, addr - offset, len);
	spin_lock_irqsave(&mapping->lock, flags);
	__fast_smmu_free_iova(mapping, addr - offset, len);
	spin_unlock_irqrestore(&mapping->lock, flags);
}

static int fast_smmu_mapping_error(struct device *dev,
				   dma_addr_t dma_addr)
{
	return dma_addr == DMA_ERROR_CODE;
}

static void __fast_smmu_mapped_over_stale(struct dma_fast_smmu_mapping *fast,
					  void *data)
{
	av8l_fast_iopte *pmds, *ptep = data;
	dma_addr_t iova;
	unsigned long bitmap_idx;
	struct io_pgtable *tbl;

	tbl  = container_of(fast->pgtbl_ops, struct io_pgtable, ops);
	pmds = tbl->cfg.av8l_fast_cfg.pmds;

	bitmap_idx = (unsigned long)(ptep - pmds);
	iova = bitmap_idx << FAST_PAGE_SHIFT;
	dev_err(fast->dev, "Mapped over stale tlb at %pa\n", &iova);
	dev_err(fast->dev, "bitmap (failure at idx %lu):\n", bitmap_idx);
	dev_err(fast->dev, "ptep: %p pmds: %p diff: %lu\n", ptep,
		pmds, bitmap_idx);
	print_hex_dump(KERN_ERR, "bmap: ", DUMP_PREFIX_ADDRESS,
		       32, 8, fast->bitmap, fast->bitmap_size, false);
}

static int fast_smmu_notify(struct notifier_block *self,
			    unsigned long action, void *data)
{
	struct dma_fast_smmu_mapping *fast = container_of(
		self, struct dma_fast_smmu_mapping, notifier);

	switch (action) {
	case MAPPED_OVER_STALE_TLB:
		__fast_smmu_mapped_over_stale(fast, data);
		return NOTIFY_OK;
	default:
		WARN(1, "Unhandled notifier action");
		return NOTIFY_DONE;
	}
}

static const struct dma_map_ops fast_smmu_dma_ops = {
	.alloc = fast_smmu_alloc,
	.free = fast_smmu_free,
	.mmap = fast_smmu_mmap_attrs,
	.get_sgtable = fast_smmu_get_sgtable,
	.map_page = fast_smmu_map_page,
	.unmap_page = fast_smmu_unmap_page,
	.sync_single_for_cpu = fast_smmu_sync_single_for_cpu,
	.sync_single_for_device = fast_smmu_sync_single_for_device,
	.map_sg = fast_smmu_map_sg,
	.unmap_sg = fast_smmu_unmap_sg,
	.sync_sg_for_cpu = fast_smmu_sync_sg_for_cpu,
	.sync_sg_for_device = fast_smmu_sync_sg_for_device,
	.map_resource = fast_smmu_dma_map_resource,
	.unmap_resource = fast_smmu_dma_unmap_resource,
	.mapping_error = fast_smmu_mapping_error,
};

/**
 * __fast_smmu_create_mapping_sized
 * @base: bottom of the VA range
 * @size: size of the VA range in bytes
 *
 * Creates a mapping structure which holds information about used/unused IO
 * address ranges, which is required to perform mapping with IOMMU aware
 * functions. The only VA range supported is [0, 4GB].
 *
 * The client device need to be attached to the mapping with
 * fast_smmu_attach_device function.
 */
static struct dma_fast_smmu_mapping *__fast_smmu_create_mapping_sized(
	dma_addr_t base, u64 size)
{
	struct dma_fast_smmu_mapping *fast;

	fast = kzalloc(sizeof(struct dma_fast_smmu_mapping), GFP_KERNEL);
	if (!fast)
		goto err;

	fast->base = base;
	fast->size = size;
	fast->num_4k_pages = size >> FAST_PAGE_SHIFT;
	fast->bitmap_size = BITS_TO_LONGS(fast->num_4k_pages) * sizeof(long);

	fast->bitmap = kzalloc(fast->bitmap_size, GFP_KERNEL | __GFP_NOWARN |
								__GFP_NORETRY);
	if (!fast->bitmap)
		fast->bitmap = vzalloc(fast->bitmap_size);

	if (!fast->bitmap)
		goto err2;

	fast->clean_bitmap = kzalloc(fast->bitmap_size, GFP_KERNEL |
				     __GFP_NOWARN | __GFP_NORETRY);
	if (!fast->clean_bitmap)
		fast->clean_bitmap = vzalloc(fast->bitmap_size);

	if (!fast->clean_bitmap)
		goto err3;

	spin_lock_init(&fast->lock);

	return fast;

err_free_bitmap:
	kvfree(fast->clean_bitmap);
err2:
	kfree(fast);
err:
	return ERR_PTR(-ENOMEM);
}

/*
 * Based off of similar code from dma-iommu.c, but modified to use a different
 * iova allocator
 */
static void fast_smmu_reserve_pci_windows(struct device *dev,
			    struct dma_fast_smmu_mapping *mapping)
{
	struct pci_host_bridge *bridge;
	struct resource_entry *window;
	phys_addr_t start, end;
	struct pci_dev *pci_dev;
	unsigned long flags;

	if (!dev_is_pci(dev))
		return;

	pci_dev = to_pci_dev(dev);
	bridge = pci_find_host_bridge(pci_dev->bus);

	spin_lock_irqsave(&mapping->lock, flags);
	resource_list_for_each_entry(window, &bridge->windows) {
		if (resource_type(window->res) != IORESOURCE_MEM &&
		    resource_type(window->res) != IORESOURCE_IO)
			continue;

		start = round_down(window->res->start - window->offset,
				FAST_PAGE_SIZE);
		end = round_up(window->res->end - window->offset,
				FAST_PAGE_SIZE);
		start = max_t(unsigned long, mapping->base, start);
		end = min_t(unsigned long, mapping->base + mapping->size, end);
		if (start >= end)
			continue;

		dev_dbg(dev, "iova allocator reserved 0x%pa-0x%pa\n",
				&start, &end);

		start = (start - mapping->base) >> FAST_PAGE_SHIFT;
		end = (end - mapping->base) >> FAST_PAGE_SHIFT;
		bitmap_set(mapping->bitmap, start, end - start);
	}
	spin_unlock_irqrestore(&mapping->lock, flags);
}

static int fast_smmu_errata_init(struct dma_iommu_mapping *mapping)
{
	struct dma_fast_smmu_mapping *fast = mapping->fast;
	int vmid = VMID_HLOS;
	int min_iova_align = 0;

	iommu_domain_get_attr(mapping->domain,
			DOMAIN_ATTR_QCOM_MMU500_ERRATA_MIN_IOVA_ALIGN,
			&min_iova_align);
	iommu_domain_get_attr(mapping->domain, DOMAIN_ATTR_SECURE_VMID, &vmid);
	if (vmid >= VMID_LAST || vmid < 0)
		vmid = VMID_HLOS;

	if (min_iova_align) {
		fast->min_iova_align = ARM_SMMU_MIN_IOVA_ALIGN;
		fast->guard_page = arm_smmu_errata_get_guard_page(vmid);
		if (!fast->guard_page)
			return -ENOMEM;
	}
	return 0;
}

/**
 * fast_smmu_init_mapping
 * @dev: valid struct device pointer
 * @mapping: io address space mapping structure (returned from
 *	arm_iommu_create_mapping)
 *
 * Called the first time a device is attached to this mapping.
 * Not for dma client use.
 */
int fast_smmu_init_mapping(struct device *dev,
			    struct dma_iommu_mapping *mapping)
{
	int err = 0;
	struct iommu_domain *domain = mapping->domain;
	struct iommu_pgtbl_info info;
	u64 size = (u64)mapping->bits << PAGE_SHIFT;

	if (mapping->base + size > (SZ_1G * 4ULL)) {
		dev_err(dev, "Iova end address too large\n");
		return -EINVAL;
	}

	mapping->fast = __fast_smmu_create_mapping_sized(mapping->base, size);
	if (IS_ERR(mapping->fast))
		return -ENOMEM;
	mapping->fast->domain = domain;
	mapping->fast->dev = dev;

	if (fast_smmu_errata_init(mapping))
		goto release_mapping;

	fast_smmu_reserve_pci_windows(dev, mapping->fast);

	domain->geometry.aperture_start = mapping->base;
	domain->geometry.aperture_end = mapping->base + size - 1;

	if (iommu_domain_get_attr(domain, DOMAIN_ATTR_PGTBL_INFO,
				  &info)) {
		dev_err(dev, "Couldn't get page table info\n");
		err = -EINVAL;
		goto release_mapping;
	}
	mapping->fast->pgtbl_ops = (struct io_pgtable_ops *)info.ops;

	mapping->fast->notifier.notifier_call = fast_smmu_notify;
	av8l_register_notify(&mapping->fast->notifier);

	mapping->ops = &fast_smmu_dma_ops;
	return 0;

release_mapping:
	kfree(mapping->fast->bitmap);
	kfree(mapping->fast);
	return err;
}

/**
 * fast_smmu_release_mapping
 * @kref: dma_iommu_mapping->kref
 *
 * Cleans up the given iommu mapping.
 */
void fast_smmu_release_mapping(struct kref *kref)
{
	struct dma_iommu_mapping *mapping =
		container_of(kref, struct dma_iommu_mapping, kref);

	kvfree(mapping->fast->bitmap);
	kfree(mapping->fast);
	iommu_domain_free(mapping->domain);
	kfree(mapping);
}
