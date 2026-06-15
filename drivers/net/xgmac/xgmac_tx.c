/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <ethdev_driver.h>
#include <stdbool.h>
#include <rte_mbuf.h>

#include "xgmac_ethdev.h"
#include "xgmac_rxtx.h"
#include "xgmac_regs.h"

static inline uint16_t
xgmac_tx_offload_flags(struct rte_eth_dev *eth_dev)
{
	uint64_t offloads = eth_dev->data->dev_conf.txmode.offloads;
	uint16_t flags = XGMAC_TX_OFFLOAD_NONE;

	if (offloads & (RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
			RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
			RTE_ETH_TX_OFFLOAD_TCP_CKSUM))
		flags |= XGMAC_TX_OFFLOAD_CKSUM;

	if (offloads & (RTE_ETH_TX_OFFLOAD_VLAN_INSERT |
			RTE_ETH_TX_OFFLOAD_QINQ_INSERT))
		flags |= XGMAC_TX_OFFLOAD_VLAN;

	if (offloads & RTE_ETH_TX_OFFLOAD_TCP_TSO)
		flags |= XGMAC_TX_OFFLOAD_TSO;

	if (offloads & RTE_ETH_TX_OFFLOAD_MULTI_SEGS)
		flags |= XGMAC_TX_MULTI_SEG;

	return flags;
}

int
xgmac_tx_offload_update(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint16_t mode = dev->tx_offload_flags;

	dev->tx_offload_flags = xgmac_tx_offload_flags(eth_dev);

	eth_dev->tx_pkt_burst = xgmac_eth_tx_burst[mode & (XGMAC_TX_MODE_MAX - 1)];
	if (eth_dev->data->dev_started)
		rte_eth_fp_ops[eth_dev->data->port_id].tx_pkt_burst = eth_dev->tx_pkt_burst;

	return 0;
}

static inline uint32_t
xgmac_tx_desc_cic_flags(const struct rte_mbuf *mbuf, uint16_t flags)
{
	uint64_t l4_mask;

	if (!(flags & XGMAC_TX_OFFLOAD_CKSUM))
		return 0;

	l4_mask = mbuf->ol_flags & RTE_MBUF_F_TX_L4_MASK;
	if (l4_mask == RTE_MBUF_F_TX_TCP_CKSUM ||
	    l4_mask == RTE_MBUF_F_TX_UDP_CKSUM)
		return XGMAC_FIELD_PREP(XGMAC_TDES3_CIC, 0x3);

	if (mbuf->ol_flags & RTE_MBUF_F_TX_IP_CKSUM)
		return XGMAC_FIELD_PREP(XGMAC_TDES3_CIC, 0x1);

	return 0;
}

static inline void
xgmac_tx_reclaim(struct xgmac_tx_queue *txq)
{
	volatile union xgmac_tx_desc *desc;
	uint16_t idx;

	while (txq->dirty != txq->cur) {
		idx = txq->dirty & (txq->nb_desc - 1);
		desc = &txq->desc[idx];

		if (desc->read.tdes3 & XGMAC_TDES3_OWN)
			break;

		desc->read.baddr = 0;
		desc->read.tdes2 = 0;
		desc->read.tdes3 = 0;
		if (txq->sw_ring[idx])
			rte_pktmbuf_free(txq->sw_ring[idx]);
		txq->sw_ring[idx] = NULL;
		txq->dirty = DESC_OFF_ADD(txq->dirty, 1, txq->nb_desc);
	}
}

static __rte_always_inline uint16_t
xgmac_xmit_pkts(void *tx_queue, struct rte_mbuf **tx_pkts, uint16_t nb_pkts,
		uint16_t flags, bool allow_mseg)
{
	struct xgmac_tx_queue *txq = tx_queue;
	struct rte_eth_dev *eth_dev = &rte_eth_devices[txq->port_id];
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	bool vlan_insert, qinq_insert, emit_vlan_ctx;
	uint16_t avail, nsegs, desc_needed, j;
	uint32_t tdes3, seg_tdes3, seg_tdes2;
	volatile union xgmac_tx_desc *desc;
	uint16_t nb_desc = txq->nb_desc;
	struct rte_mbuf *mbuf, *seg;
	uint16_t sent = 0, idx = 0;
	uint64_t cur = txq->cur;
	uint16_t outer_tci;
	uint32_t csum_cic;

	if (unlikely(nb_pkts == 0))
		return nb_pkts;

	avail = xgmac_desc_avail(cur, txq->dirty, nb_desc);
	if (avail <= txq->free_thresh) {
		xgmac_tx_reclaim(txq);
		avail = xgmac_desc_avail(cur, txq->dirty, nb_desc);
	}

	if (!avail)
		return 0;

	while (sent < nb_pkts && avail > 0) {
		mbuf = tx_pkts[sent];
		nsegs = mbuf->nb_segs;
		csum_cic = xgmac_tx_desc_cic_flags(mbuf, flags);
		vlan_insert = (flags & XGMAC_TX_OFFLOAD_VLAN) &&
			      (mbuf->ol_flags & (RTE_MBUF_F_TX_VLAN | RTE_MBUF_F_TX_QINQ));
		qinq_insert = vlan_insert && (mbuf->ol_flags & RTE_MBUF_F_TX_QINQ);
		outer_tci = qinq_insert ? mbuf->vlan_tci_outer : mbuf->vlan_tci;
		emit_vlan_ctx = false;
		if (vlan_insert) {
			/* Per HW behavior, inner VLAN control must be refreshed per packet. */
			if (qinq_insert) {
				emit_vlan_ctx = true;
			} else if (!txq->vlan_ctx_valid ||
				   txq->vlan_ctx_qinq != (uint8_t)qinq_insert ||
				   txq->vlan_ctx_outer_tci != outer_tci) {
				emit_vlan_ctx = true;
			}
		}
		desc_needed = nsegs + (emit_vlan_ctx ? 1 : 0);

		if (nsegs == 0 || (!allow_mseg && nsegs != 1)) {
			txq->errors++;
			break;
		}
		if (desc_needed > avail)
			break;

		idx = cur & (nb_desc - 1);
		if (emit_vlan_ctx) {
			desc = &txq->desc[idx];
			desc->read.baddr = 0;
			desc->read.tdes2 = 0;
			seg_tdes3 = XGMAC_TDES3_CTXT | XGMAC_TDES3_VLTV |
				    XGMAC_FIELD_PREP(XGMAC_TDES3_VT, outer_tci);
			if (qinq_insert) {
				desc->read.tdes2 |= XGMAC_FIELD_PREP(XGMAC_TDES2_IVT,
								     mbuf->vlan_tci);
				seg_tdes3 |= XGMAC_TDES3_IVLTV |
					     XGMAC_FIELD_PREP(XGMAC_TDES3_IVTIR,
							      XGMAC_TDES3_IVTIR_INSERT);
			}
			desc->read.tdes3 = seg_tdes3;
			txq->sw_ring[idx] = NULL;
			txq->vlan_ctx_valid = 1;
			txq->vlan_ctx_qinq = (uint8_t)qinq_insert;
			txq->vlan_ctx_outer_tci = outer_tci;
			txq->vlan_ctx_inner_tci = qinq_insert ? mbuf->vlan_tci : 0;
			idx = DESC_OFF_ADD(idx, 1, nb_desc);
		}

		seg = mbuf;
		for (j = 0; j < nsegs; j++) {
			desc = &txq->desc[idx];
			desc->read.baddr = rte_mbuf_data_iova(seg);
			seg_tdes2 = XGMAC_FIELD_PREP(XGMAC_TDES2_B1L, seg->data_len);
			if (j == 0 && vlan_insert)
				seg_tdes2 |= XGMAC_FIELD_PREP(XGMAC_TDES2_VTIR,
							      XGMAC_TDES2_VTIR_INSERT);
			desc->read.tdes2 = seg_tdes2;
			seg_tdes3 = 0;
			if (j == 0) {
				seg_tdes3 = XGMAC_FIELD_PREP(XGMAC_TDES3_FL, mbuf->pkt_len);
				seg_tdes3 |= XGMAC_TDES3_FD;
				seg_tdes3 |= csum_cic;
			}
			if (j == (nsegs - 1))
				seg_tdes3 |= XGMAC_TDES3_LD;
			desc->read.tdes3 = seg_tdes3;

			txq->sw_ring[idx] = (j == 0) ? mbuf : NULL;
			idx = DESC_OFF_ADD(idx, 1, nb_desc);
			seg = seg->next;
		}

		rte_wmb();
		idx = cur & (nb_desc - 1);
		for (j = 0; j < desc_needed; j++) {
			desc = &txq->desc[idx];
			tdes3 = desc->read.tdes3;
			desc->read.tdes3 = tdes3 | XGMAC_TDES3_OWN;
			idx = DESC_OFF_ADD(idx, 1, nb_desc);
		}

		txq->nb_bytes += mbuf->pkt_len;
		cur = DESC_OFF_ADD(cur, desc_needed, nb_desc);
		avail -= desc_needed;
		sent++;
	}
	txq->nb_pkts += sent;
	txq->cur = cur;

	rte_wmb();
	idx = cur & (nb_desc - 1);
	if (sent > 0) {
		xgmac_wr(dev, XGMAC_DMA_CH_TxDESC_TAIL_LPTR(txq->queue_id),
			 xgmac_low32(txq->ring_phys_addr + (uint64_t)idx *
				     sizeof(union xgmac_tx_desc)));
	}

	return sent;
}

#define E(name, flags) \
	static uint16_t __rte_noinline __rte_hot xgmac_xmit_pkts_##name(void *tx_queue, \
			struct rte_mbuf **tx_pkts, uint16_t nb_pkts) \
	{ \
		return xgmac_xmit_pkts(tx_queue, tx_pkts, nb_pkts, flags, \
			((flags) & XGMAC_TX_MULTI_SEG) != 0); \
	}
XGMAC_TX_FASTPATH_MODES
#undef E

const eth_tx_burst_t xgmac_eth_tx_burst[XGMAC_TX_MODE_MAX] = {
#define E(name, flags)[flags] = xgmac_xmit_pkts_##name,
	XGMAC_TX_FASTPATH_MODES
#undef E
};
