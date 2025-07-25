/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2025 Marvell.
 */
#ifndef __INCLUDE_CNXK_EMDEV_DMA_H__
#define __INCLUDE_CNXK_EMDEV_DMA_H__

#include <rte_mempool.h>
#include <rte_vect.h>

#include "cnxk_emdev.h"

/**
 * Wait till all DMA instructions are completed
 * Called from non-data path core
 */
static __rte_always_inline int
cnxk_emdev_dma_compl_wait(struct cnxk_emdev_dpi_q *q, uint16_t tmo_ms)
{
	uint16_t wr_idx = plt_read64(q->widx_r) & 0xFFF;

	/* No pending in-flight instructions */
	while (!((plt_read64(q->ridx_r) >> 63) && (q->compl_idx == wr_idx))) {
		tmo_ms--;
		if (!tmo_ms)
			return -EFAULT;
		rte_delay_us_sleep(1000);
	}

	return 0;
}

/**
 * Get available space in DMA vchan state
 */
static __rte_always_inline uint16_t
cnxk_emdev_dma_avail(struct cnxk_emdev_dpi_q *q, uint16_t *idx)
{
	uint16_t widx = plt_read64(q->widx_r) & 0xFFF;
	uint16_t compl_idx = q->compl_idx;
	uint16_t q_sz = ROC_EMDEV_DPI_Q_SZ;
	uint16_t used;

	used = widx >= compl_idx ? widx - compl_idx : widx + q_sz - compl_idx;
	*idx = widx & 0xFFF;
	return q_sz - used;
}

static __rte_always_inline uint16_t
cnxk_emdev_dma_inst_idx(struct cnxk_emdev_dpi_q *q)
{
	uint64_t widx = plt_read64(q->widx_r);

	return widx & 0xFFF;
}

static __rte_always_inline uint64_t *
cnxk_emdev_dma_inst_addr(uint64_t *base, uint16_t idx)
{
	/* Assuming 64B instruction size */
	return base + (idx << 3);
}

static __rte_always_inline uint64_t *
cnxk_emdev_dma_compl_addr(uint64_t *base, uint16_t idx)
{
	/* Assuming 128B completion addr */
	return base + (idx << 4);
}

static __rte_always_inline uint64_t *
cnxk_emdev_dma_ptr_addr(uint64_t *base, uint16_t idx)
{
	/* Assuming 64B instruction size, return pointer to DPI_DMA_PTR_S */
	return cnxk_emdev_dma_inst_addr(base, idx) + 2;
}

static __rte_always_inline uint16_t
cnxk_emdev_dma_next_idx(uint16_t idx)
{
	return (idx + 1) & (ROC_EMDEV_DPI_Q_SZ - 1);
}

/**
 * Enqueue one DMA pointer pair.
 *
 */
static __rte_always_inline void
cnxk_emdev_dma_enq_x1(uint64_t *inst_base, uint64_t *compl_base, uint64_t mdata, rte_iova_t src,
		      rte_iova_t dst, uint16_t len)
{
	uint64_t w0 = (1ULL << 63 | (mdata & 0x1FFFUL) << 12 | ((mdata >> 32) << 30));
	uint64_t fp_l = (mdata >> 13) & 0x1UL;
	uint64_t fp_h = (mdata >> 14) & 0x1UL;

	/* DPI_DMA_64B_INSTR_HDR_S */
	inst_base[0] = w0 | 0x11UL;
	inst_base[1] = (uintptr_t)compl_base;
	inst_base[2] = (uint64_t)len << 32 | len | (fp_h << 63) | (fp_l << 31);
	inst_base[3] = src;
	inst_base[4] = dst;

	*(uint8_t *)compl_base = 0xFF;
}

/**
 * Enqueue multiple DMA pointers.
 */
#if defined(RTE_ARCH_ARM64)
static __rte_always_inline void
cnxk_emdev_dma_enq_xn(uint64_t *inst_base, uint64_t *compl_base, uint64_t mdata, uint64x2_t *vsrc,
		      uint64x2_t *vdst, uint8_t nsrc, uint8_t ndst)
{
	PLT_SET_USED(inst_base);
	PLT_SET_USED(compl_base);
	PLT_SET_USED(mdata);
	PLT_SET_USED(vsrc);
	PLT_SET_USED(vdst);
	PLT_SET_USED(nsrc);
	PLT_SET_USED(ndst);
}
#endif

#endif /* __INCLUDE_CNXK_EMDEV_DMA_H__ */
