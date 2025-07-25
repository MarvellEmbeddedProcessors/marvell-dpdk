/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2025 Marvell.
 */

#include <bus_pci_driver.h>
#include <dev_driver.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_pci.h>
#include <rte_rawdev.h>
#include <rte_rawdev_pmd.h>

#include <roc_api.h>

#include "cnxk_emdev_dma.h"
#include "cnxk_emdev_vnet.h"

int
cnxk_emdev_vnet_ctrl_deq_psw_dbl(struct cnxk_emdev_queue *queue,
				 struct cnxk_emdev_vnet_queue *vnet_q, uint16_t pi,
				 const uint16_t flags)
{
	struct cnxk_emdev_dpi_q *inb_q = &queue->dpi_q_inb;
	uint64_t *compl_base = inb_q->compl_base;
	uint64_t *dma_base = inb_q->inst_base, *dma_ptr;
	struct rte_pmd_cnxk_emdev_event *event;
	uintptr_t sd_base = vnet_q->sd_base;
	uint16_t q_sz = vnet_q->q_sz;
	uint16_t nb_desc, ci, off, segs = 0;
	uint64_t *compl_ptr = NULL;
	uint16_t avail, dma_idx;
	struct rte_mbuf *mbuf;
	uint32_t tot_len = 0;
	uint64_t dflags, val;
	uint64_t desc_flag;
	uint32_t i, len;
	uint16_t tmo_ms;
	uint32_t space;

	PLT_SET_USED(flags);
	ci = vnet_q->ci_desc;
#ifdef CNXK_EMDEV_DEBUG
	plt_emdev_dbg("[dev %u] CQ desc ci: %u pi:%u", vnet_q->epf_func, ci, pi);
#endif

	/* Return if there is nothing to process */
	nb_desc = wrap_off_diff(pi, ci, vnet_q->q_sz);
	if (!nb_desc)
		return 0;

	/* Skip if there is no space to save the event */
	space = CNXK_EMDEV_Q_MBUF_RING_SZ - 1;
	space -= DESC_DIFF(queue->mbuf_pi, queue->mbuf_ci, CNXK_EMDEV_Q_MBUF_RING_SZ);
	if (space < nb_desc)
		return 0;

	plt_emdev_dbg("[dev %u] CQ desc nb_desc: %u", vnet_q->epf_func, nb_desc);
#ifdef CNXK_EMDEV_DEBUG
	rte_hexdump(stdout, "CQ descriptors", VNET_DESC_PTR_OFF(sd_base, ci, 0), nb_desc * 16);
#endif

	off = ci;
	do {
		dflags = (*VNET_DESC_PTR_OFF(sd_base, off, 8) >> VRING_DESC_F_NEXT) & 0x1;
		off = wrap_off_add(off, 1, q_sz);
		segs++;
	} while (dflags);

	/* Check if there is space in DPI ring */
	avail = cnxk_emdev_dma_avail(inb_q, &dma_idx);
	if (avail < segs)
		return -ENOSPC;

	mbuf = rte_pktmbuf_alloc(vnet_q->mp);
	if (!mbuf)
		return -ENOMEM;

	off = ci;
	for (i = 0; i < segs; i++) {
		dma_ptr = cnxk_emdev_dma_inst_addr(dma_base, dma_idx);
		compl_ptr = cnxk_emdev_dma_compl_addr(compl_base, dma_idx);

		len = *VNET_DESC_PTR_OFF(sd_base, off, 8) & (RTE_BIT64(32) - 1);

		/* Mark the descriptor as used */
		dflags = *VNET_DESC_PTR_OFF(sd_base, off, 8);
		desc_flag = !!(dflags & VIRT_PACKED_RING_DESC_F_AVAIL);
		dflags &= ~VIRT_PACKED_RING_DESC_F_AVAIL_USED;
		*VNET_DESC_PTR_OFF(sd_base, off, 8) = dflags | desc_flag << 55 | desc_flag << 63;

		/* Submit a DMA instruction */
		cnxk_emdev_dma_enq_x1(dma_ptr, compl_ptr, 0, *VNET_DESC_PTR_OFF(sd_base, off, 0),
				      rte_pktmbuf_mtod_offset(mbuf, rte_iova_t, tot_len), len);
		tot_len += len;
		dma_idx = cnxk_emdev_dma_next_idx(dma_idx);
		off = wrap_off_add(off, 1, q_sz);
	}
	mbuf->data_len = tot_len + sizeof(struct rte_pmd_cnxk_emdev_event);
	mbuf->pkt_len = mbuf->data_len;
	mbuf->data_off -= sizeof(struct rte_pmd_cnxk_emdev_event);

	/* Trigger DMA */
	plt_io_wmb();
	plt_write64(nb_desc, inb_q->widx_r);

	/* Wait for the DMA to complete */
	tmo_ms = CNXK_EMDEV_DMA_TMO_MS;
	do {
		rte_delay_us_sleep(1000);
		__atomic_load(&compl_ptr[0], &val, __ATOMIC_ACQUIRE);
		tmo_ms--;
		if (!tmo_ms) {
			plt_err("[dev 0x%x] ctrl deq DMA timeout", vnet_q->epf_func);
			rte_pktmbuf_free(mbuf);
			return -EFAULT;
		}
	} while (val == 0xFF);

	/* Populate event info */
	event = rte_pktmbuf_mtod(mbuf, struct rte_pmd_cnxk_emdev_event *);
	event->func_id = vnet_q->epf_func & 0xFF;
	event->qid = vnet_q->qid;
	event->ci_start = ci;
	event->ci_end = off;
	event->type = RTE_PMD_CNXK_EMDEV_EVENT_TYPE_CTRL;
	event->data_len = tot_len;

	/* Store queue and vf_id and qid info in fdir */
	mbuf->port = vnet_q->epf_func;
	mbuf->hash.fdir.id = vnet_q->qid;
	/* Store the event in the mbuf ring */
	queue->mbuf_arr[queue->mbuf_pi] = mbuf;
	queue->mbuf_pi = DESC_ADD(queue->mbuf_pi, 1, CNXK_EMDEV_Q_MBUF_RING_SZ);

	/* Change the shadow queue descriptor flag to USED */
	*VNET_DESC_PTR_OFF(sd_base, off, 8) = VRING_DESC_F_WRITE;
	*VNET_DESC_PTR_OFF(sd_base, off, 8) |= VIRT_PACKED_RING_DESC_F_AVAIL_USED;

	vnet_q->pi_desc = pi;
	vnet_q->ci_desc = off;
	return 0;
}

int
cnxk_emdev_vnet_deq_dpi_compl(struct cnxk_emdev_queue *queue, struct cnxk_emdev_vnet_queue *vnet_q,
			      uint16_t comp_idx, const uint16_t flags)
{
	const uint64_t rearm_data = 0x100010000ULL | RTE_PKTMBUF_HEADROOM;
	struct cnxk_emdev_dpi_q *inb_q = &queue->dpi_q_inb;
	struct rte_mbuf **mbuf_arr = queue->mbuf_arr;
	uint64_t *compl_base = inb_q->compl_base;
	struct cnxk_emdev_psw_q *aq = &queue->aq;
	uint16_t vhdr_sz = vnet_q->virtio_hdr_sz;
	uint16_t ci_start, ci_desc, aq_pi, aq_ci;
	uint64_t *dma_base = inb_q->inst_base;
	uintptr_t sd_base = vnet_q->sd_base;
	uint64_t *aq_pi_dbl, *aq_ci_dbl;
	uint64_t last_id, first, used;
	uint16_t q_sz = vnet_q->q_sz;
	uint16_t dma_idx_s, dma_idx;
	uint64_t *compl_ptr, data;
	uint16_t mbuf_ci, mbuf_pi;
	uint64_t ack_desc, space;
	struct rte_mbuf *mbuf;
	uint64_t *dma_ptr;
	uint16_t count;

	/* Check if we have space in AQ */
	aq_pi_dbl = (uint64_t *)aq->pi_dbl;
	aq_ci_dbl = (uint64_t *)aq->ci_dbl;
	aq_pi = plt_read64(aq_pi_dbl);
	aq_ci = plt_read64(aq_ci_dbl);

	/* Check if Ack queue is full */
	if (DESC_ADD(aq_pi, 1, aq->q_sz) == aq_ci)
		return -ENOSPC;

	/* Prepare PSW_ACK_DOORBELL_DESC_S */
	compl_ptr = cnxk_emdev_dma_compl_addr(compl_base, comp_idx);
	data = compl_ptr[2];
	ci_start = (data >> 16) & 0xFFFF;
	ci_desc = data & 0xFFFF;
	count = wrap_off_diff(ci_desc, ci_start, q_sz);

	/* Check if we have space in mbuf ring */
	mbuf_ci = queue->mbuf_ci;
	mbuf_pi = queue->mbuf_pi;
	space = (CNXK_EMDEV_Q_MBUF_RING_SZ - 1) -
		DESC_DIFF(mbuf_pi, mbuf_ci, CNXK_EMDEV_Q_MBUF_RING_SZ);
	if (space < count)
		return -ENOSPC;

	/* Populate mbufs  */
	dma_idx = (data >> 32) & 0xFFFF;
	dma_idx_s = (data >> 48);

	while (dma_idx_s != dma_idx) {
		/* Get mbuf from DPI_DMA_PTR_S */
		dma_ptr = cnxk_emdev_dma_ptr_addr(dma_base, dma_idx_s);

		mbuf = (struct rte_mbuf *)((uintptr_t)dma_ptr[2] - vnet_q->data_off);
		data = dma_ptr[0] & 0xFFFFFF;
		*((uint64_t *)&mbuf->rearm_data) = rearm_data + vhdr_sz;
		mbuf->pkt_len = data - vhdr_sz;
		mbuf->data_len = data - vhdr_sz;
		mbuf->next = NULL;
		mbuf->ol_flags = 0;
		mbuf->port = vnet_q->epf_func;
		mbuf->hash.fdir.id = vnet_q->qid;

		/* Store mbuf in ring */
		mbuf_arr[mbuf_pi] = mbuf;
		mbuf_pi = DESC_ADD(mbuf_pi, 1, CNXK_EMDEV_Q_MBUF_RING_SZ);

		dma_idx_s = cnxk_emdev_dma_next_idx(dma_idx_s);
	}
	queue->mbuf_pi = mbuf_pi;
	vnet_q->ci_desc = ci_desc;

	if (!(flags & DPI_DEQ_NOINORDER_F)) {
		last_id = *VNET_DESC_PTR_OFF(sd_base, wrap_off_m1(ci_desc, q_sz), 8) >> 32;
		last_id &= 0xFFFF;
		first = *VNET_DESC_PTR_OFF(sd_base, ci_start, 8) & ~0x8000FFFF00000000UL;
		used = (first >> 55) & 0x1;
		*VNET_DESC_PTR_OFF(sd_base, ci_start, 8) = first | (used << 63) | (last_id << 32);
	}

	/* Prepare PSW_ACK_DOORBELL_DESC_S */
	ack_desc = (uint64_t)ci_desc << 32;
	ack_desc |= (uint64_t)vnet_q->qid << 8;
	ack_desc |= (uint64_t)vnet_q->epf_func << 16;
	ack_desc |= (uint64_t)wrap_off_diff(ci_desc, ci_start, vnet_q->q_sz) << 48;

	/* Add instruction to ack queue to trigger descriptor store */
	*AQ_DESC_PTR_OFF(aq->q_base, aq_pi, 0) = ack_desc;
	aq_pi = DESC_ADD(aq_pi, 1, aq->q_sz);
	plt_io_wmb();
	plt_write64(aq_pi, aq_pi_dbl);

	return 0;
}

int
cnxk_emdev_vnet_deq_psw_dbl(struct cnxk_emdev_queue *queue, struct cnxk_emdev_vnet_queue *vnet_q,
			    uint16_t pi, const uint16_t flags)
{
	struct cnxk_emdev_dpi_q *inb_q = &queue->dpi_q_inb;
	uint64_t *compl_base = inb_q->compl_base;
	uint16_t avail, dma_idx, i, dma_idx_s;
	uint64_t *dma_base = inb_q->inst_base;
	uintptr_t sd_base = vnet_q->sd_base;
	uint32_t buf_len, slen, dlen, pend;
	uint16_t pi_desc = vnet_q->pi_desc;
	uint64_t *compl_ptr, *dma_ptr;
	uint16_t q_sz = vnet_q->q_sz;
	uint64_t d_flags, mdata;
	uint16_t dma_cnt = 0;
	uint16_t nb_desc;
	uint64_t used;

	/* Check space in DPI ring and skip consuming the desc if DPI queue is full */
	avail = cnxk_emdev_dma_avail(inb_q, &dma_idx);
	if (!avail)
		return -ENOSPC;

	/* Return is no descriptor avail */
	nb_desc = wrap_off_diff(pi, pi_desc, q_sz);
	if (!nb_desc)
		return 0;

#ifdef CNXK_EMDEV_DEBUG
	rte_hexdump(stdout, "Host Tx queue descriptors", VNET_DESC_PTR_OFF(sd_base, pi_desc, 0),
		    nb_desc * 16);
#endif
	nb_desc = RTE_MIN(nb_desc, avail << 1);

	nb_desc = RTE_MIN(nb_desc, ROC_EMDEV_PSW_BURST_SZ);

	pi = wrap_off_add(pi_desc, nb_desc, q_sz);

	buf_len = vnet_q->buf_len;

	dma_ptr = cnxk_emdev_dma_inst_addr(dma_base, dma_idx);
	compl_ptr = cnxk_emdev_dma_compl_addr(compl_base, dma_idx);
	dma_idx_s = dma_idx;

	/* Process the descriptors */
	mdata = (1 << 14) | roc_npa_aura_handle_to_aura(vnet_q->mp->pool_id) << 32;
	i = pi_desc;
	while (i != WRAP_OFF(pi)) {

		d_flags = *VNET_DESC_PTR_OFF(sd_base, i, 8);
		slen = d_flags & (RTE_BIT64(32) - 1);
		dlen = slen;

		if (unlikely(slen > buf_len)) {
			pend = slen - buf_len;
			dlen = buf_len;
		}

		if (flags & DBL_DEQ_NOINORDER_F) {
			used = (d_flags >> 55) & 0x1;
			d_flags = d_flags & ~RTE_BIT64(63);
			d_flags |= used << 63;
			*VNET_DESC_PTR_OFF(sd_base, i, 8) = d_flags;
		}

		/* Enqueue req to DPI */
		dma_ptr = cnxk_emdev_dma_inst_addr(dma_base, dma_idx);
		compl_ptr = cnxk_emdev_dma_compl_addr(compl_base, dma_idx);
		cnxk_emdev_dma_enq_x1(dma_ptr, compl_ptr, mdata, *VNET_DESC_PTR_OFF(sd_base, i, 0),
				      (rte_iova_t)NULL, dlen);
		compl_ptr[1] = 0;
		dma_idx = cnxk_emdev_dma_next_idx(dma_idx);
		dma_cnt++;
		i = (i + 1) & (q_sz - 1);
		PLT_SET_USED(pend);
	}

	if (likely(dma_cnt)) {
		/* Store vnet_q in the last completion to get callback after that */
		compl_ptr[1] = (uintptr_t)vnet_q;
		compl_ptr[2] = (uint64_t)pi_desc << 16 | pi;
		compl_ptr[2] |= (uint64_t)dma_idx_s << 48 | (uint64_t)dma_idx << 32;
		/* Issue a doorbell to trigger DMA */
		plt_io_wmb();
		plt_write64(dma_cnt, inb_q->widx_r);
	}

	vnet_q->pi_desc = pi;
	/* Return success only if all descriptors are sent to next stage */
	return !(i == WRAP_OFF(pi));
}

int
cnxk_emdev_vnet_dequeue(struct rte_rawdev *rawdev, struct rte_rawdev_buf **bufs, uint32_t count,
			rte_rawdev_obj_t ctx)
{
	struct cnxk_emdev *dev = cnxk_rawdev_priv(rawdev);
	uint32_t mbuf_pi, mbuf_ci, max, nb_pkts;
	struct cnxk_emdev_dpi_q *dpi_q;
	struct cnxk_emdev_queue *queue;
	struct rte_mbuf **mbuf_arr;
	uint16_t qid;

	qid = cnxk_emdev_qid_from_ctx(ctx);
	queue = &dev->emdev_qs[qid];

	/* Process doorbell descriptors */
	emdev_dbl_desc_process(queue);

	dpi_q = &queue->dpi_q_inb;
	/* Process DPI inbound completions */
	emdev_dpi_compl_process(queue, dpi_q);

	mbuf_pi = queue->mbuf_pi;
	mbuf_ci = queue->mbuf_ci;

	max = DESC_DIFF(mbuf_pi, mbuf_ci, CNXK_EMDEV_Q_MBUF_RING_SZ);
	/* Check if there are processed mbufs */
	if (!max)
		return 0;

	mbuf_arr = queue->mbuf_arr;
	count = RTE_MIN(count, max);

	/* Copy processed mbufs from ring to return */
	nb_pkts = (mbuf_ci + count) > CNXK_EMDEV_Q_MBUF_RING_SZ ?
			  CNXK_EMDEV_Q_MBUF_RING_SZ - mbuf_ci :
			  count;
	rte_memcpy(bufs, (struct rte_rawdev_buf **)&mbuf_arr[mbuf_ci],
		   nb_pkts * sizeof(struct rte_mbuf *));
	nb_pkts = count - nb_pkts;
	if (nb_pkts) {
		rte_memcpy(bufs + nb_pkts, (struct rte_rawdev_buf **)&mbuf_arr[0],
			   nb_pkts * sizeof(struct rte_mbuf *));
	}

	mbuf_ci = DESC_ADD(mbuf_ci, count, CNXK_EMDEV_Q_MBUF_RING_SZ);
	queue->mbuf_ci = mbuf_ci;
	return count;
}
