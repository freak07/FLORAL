/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
 */

#ifndef __LINUX_DMA_MAPPING_FAST_H
#define __LINUX_DMA_MAPPING_FAST_H

#include <linux/iommu.h>
#include <linux/io-pgtable-fast.h>

struct dma_iommu_mapping;
struct io_pgtable_ops;

struct dma_fast_smmu_mapping {
	struct device		*dev;
	struct iommu_domain	*domain;
	dma_addr_t	 base;
	size_t		 size;
	size_t		 num_4k_pages;

	u32		min_iova_align;
	struct page	*guard_page;

	unsigned int	bitmap_size;
	/* bitmap has 1s marked only valid mappings */
	unsigned long	*bitmap;
	/* clean_bitmap has 1s marked for both valid and stale tlb mappings */
	unsigned long	*clean_bitmap;
	unsigned long	next_start;
	bool		have_stale_tlbs;

	dma_addr_t	pgtbl_dma_handle;
	struct io_pgtable_ops *pgtbl_ops;

	spinlock_t	lock;
	struct notifier_block notifier;
};

#ifdef CONFIG_IOMMU_IO_PGTABLE_FAST
int fast_smmu_init_mapping(struct device *dev,
			    struct dma_iommu_mapping *mapping);
void fast_smmu_release_mapping(struct kref *kref);
#else
static inline int fast_smmu_init_mapping(struct device *dev,
					  struct dma_iommu_mapping *mapping)
{
	return -ENODEV;
}

static inline void fast_smmu_release_mapping(struct kref *kref)
{
}
#endif

#endif /* __LINUX_DMA_MAPPING_FAST_H */
