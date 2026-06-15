/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <ethdev_driver.h>
#include <errno.h>
#include <stdbool.h>
#include <rte_mbuf.h>

#include "xgmac_dev.h"
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
	uint64_t offloads = eth_dev->data->dev_conf.txmode.offloads;
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_tx_queue *txq;
	bool tso_enabled;
	bool tnl_enabled;
	uint32_t val;
	uint16_t q;

	dev->tx_offload_flags = xgmac_tx_offload_flags(eth_dev);

	if (dev->hw_feat.tso) {
		tso_enabled = !!(dev->tx_offload_flags & XGMAC_TX_OFFLOAD_TSO);
		for (q = 0; q < eth_dev->data->nb_tx_queues; q++) {
			txq = eth_dev->data->tx_queues[q];
			if (!txq)
				continue;
			if (eth_dev->data->dev_started) {
				val = xgmac_rd(dev, XGMAC_DMA_CH_TX_CONTROL(txq->queue_id));
				if (tso_enabled)
					val |= XGMAC_TSE;
				else
					val &= ~XGMAC_TSE;
				xgmac_wr(dev, XGMAC_DMA_CH_TX_CONTROL(txq->queue_id), val);
			}
			if (!tso_enabled)
				txq->tso_mss_valid = 0;
		}
	}

	if (dev->hw_feat.tunnel && eth_dev->data->dev_started) {
		bool nvgre_mode = !!(offloads & RTE_ETH_TX_OFFLOAD_GRE_TNL_TSO);

		tnl_enabled = !!(offloads & (RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO |
					     RTE_ETH_TX_OFFLOAD_GRE_TNL_TSO |
					     RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM |
					     RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM));
		val = xgmac_rd(dev, XGMAC_TX_CONFIG);
		val &= ~(XGMAC_CONFIG_VNE | XGMAC_CONFIG_VNM);
		if (tnl_enabled) {
			val |= XGMAC_CONFIG_VNE;
			if (nvgre_mode)
				val |= XGMAC_CONFIG_VNM; /* NVGRE mode */
		}
		xgmac_wr(dev, XGMAC_TX_CONFIG, val);
	}

	eth_dev->tx_pkt_burst = xgmac_eth_tx_burst[dev->tx_offload_flags & (XGMAC_TX_MODE_MAX - 1)];
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

static __rte_always_inline int
xgmac_tx_fill_tso_descs(struct xgmac_tx_queue *txq, struct rte_mbuf *mbuf, uint16_t nb_desc,
			uint16_t *idx, bool vlan_insert, bool tunnel,
			uint32_t csum_cic, uint32_t hdr_len, uint32_t payload_len)
{
	uint32_t remaining_payload = payload_len;
	uint32_t seg_off = hdr_len, seg_len;
	volatile union xgmac_tx_desc *desc;
	uint32_t seg_tdes2, seg_tdes3;
	struct rte_mbuf *seg = mbuf;
	uint16_t filled = 0;

	desc = &txq->desc[*idx];
	desc->read.baddr = rte_mbuf_data_iova(seg);
	seg_tdes2 = XGMAC_FIELD_PREP(XGMAC_TDES2_B1L, hdr_len);
	if (vlan_insert)
		seg_tdes2 |= XGMAC_FIELD_PREP(XGMAC_TDES2_VTIR, XGMAC_TDES2_VTIR_INSERT);
	desc->read.tdes2 = seg_tdes2;
	seg_tdes3 = XGMAC_TDES3_FD | XGMAC_TDES3_TSE | csum_cic |
		    XGMAC_FIELD_PREP(XGMAC_TDES3_TPL, payload_len) |
		    XGMAC_FIELD_PREP(XGMAC_TDES3_THL, mbuf->l4_len / 4);
	if (tunnel)
		seg_tdes3 |= XGMAC_FIELD_PREP(XGMAC_TDES3_VNP, XGMAC_TDES3_VNP_TUNNEL);
	desc->read.tdes3 = seg_tdes3;
	txq->sw_ring[*idx] = mbuf;
	*idx = DESC_OFF_ADD(*idx, 1, nb_desc);
	filled++;

	while (remaining_payload && seg) {
		if (seg_off >= seg->data_len) {
			seg = seg->next;
			seg_off = 0;
			continue;
		}
		seg_len = RTE_MIN((uint32_t)(seg->data_len - seg_off), remaining_payload);
		if (!seg_len) {
			seg = seg->next;
			seg_off = 0;
			continue;
		}
		desc = &txq->desc[*idx];
		desc->read.baddr = rte_mbuf_data_iova(seg) + seg_off;
		desc->read.tdes2 = XGMAC_FIELD_PREP(XGMAC_TDES2_B1L, seg_len);
		seg_tdes3 = 0;
		remaining_payload -= seg_len;
		if (!remaining_payload)
			seg_tdes3 |= XGMAC_TDES3_LD;
		desc->read.tdes3 = seg_tdes3;
		txq->sw_ring[*idx] = NULL;
		*idx = DESC_OFF_ADD(*idx, 1, nb_desc);
		filled++;

		seg = seg->next;
		seg_off = 0;
	}

	return remaining_payload ? -EINVAL : filled;
}

static __rte_always_inline uint16_t
xgmac_xmit_pkts(void *tx_queue, struct rte_mbuf **tx_pkts, uint16_t nb_pkts,
		uint16_t flags, bool allow_mseg)
{
	uint16_t avail, nsegs, desc_needed, j, desc_filled, tso_payload_desc;
	struct xgmac_tx_queue *txq = tx_queue;
	struct rte_eth_dev *eth_dev = &rte_eth_devices[txq->port_id];
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	bool vlan_insert, qinq_insert, emit_vlan_ctx;
	uint32_t hdr_len = 0, payload_len = 0;
	uint32_t tdes3, seg_tdes3, seg_tdes2;
	volatile union xgmac_tx_desc *desc;
	uint32_t seg_off = 0, seg_len = 0;
	uint16_t nb_desc = txq->nb_desc;
	uint32_t remaining_payload = 0;
	struct rte_mbuf *mbuf, *seg;
	uint16_t sent = 0, idx = 0;
	uint64_t cur = txq->cur;
	uint16_t outer_tci;
	uint32_t csum_cic;
	bool tso_tunnel;
	int tso_filled;
	uint32_t ctrl;
	bool tso;

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
		tso = !!((flags & XGMAC_TX_OFFLOAD_TSO) &&
			 (mbuf->ol_flags & RTE_MBUF_F_TX_TCP_SEG));
		tso_tunnel = tso && !!(mbuf->ol_flags & RTE_MBUF_F_TX_TUNNEL_MASK);
		hdr_len = 0;
		payload_len = 0;
		tso_payload_desc = 0;
		desc_filled = 0;
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
		if (tso) {
			if (tso_tunnel) {
				hdr_len = (uint32_t)mbuf->outer_l2_len +
					  (uint32_t)mbuf->outer_l3_len +
					  (uint32_t)mbuf->l2_len +
					  (uint32_t)mbuf->l3_len +
					  (uint32_t)mbuf->l4_len;
			} else {
				hdr_len = (uint32_t)mbuf->l2_len + (uint32_t)mbuf->l3_len +
					  (uint32_t)mbuf->l4_len;
			}
			payload_len = mbuf->pkt_len - hdr_len;
			if (unlikely(hdr_len == 0 || hdr_len > XGMAC_TSO_MAX_HDR_LEN ||
				     hdr_len >= mbuf->pkt_len ||
				     mbuf->data_len < hdr_len ||
				     mbuf->tso_segsz < XGMAC_TSO_MIN_MSS ||
				     mbuf->tso_segsz > XGMAC_TSO_MAX_MSS ||
				     hdr_len + mbuf->tso_segsz > txq->tso_max_seg_len ||
				     payload_len > XGMAC_TSO_MAX_PAYLOAD ||
				     mbuf->l4_len < XGMAC_TSO_MIN_TCP_HDR ||
				     mbuf->l4_len > XGMAC_TSO_MAX_TCP_HDR ||
				     (mbuf->l4_len & 0x3))) {
				txq->errors++;
				txq->tso_rejected++;
				rte_pktmbuf_free(mbuf);
				sent++;
				continue;
			}

			remaining_payload = payload_len;
			seg = mbuf;
			seg_off = hdr_len;
			while (remaining_payload && seg) {
				if (seg_off >= seg->data_len) {
					seg = seg->next;
					seg_off = 0;
					continue;
				}
				seg_len = RTE_MIN((uint32_t)(seg->data_len - seg_off),
						  remaining_payload);
				if (seg_len) {
					tso_payload_desc++;
					remaining_payload -= seg_len;
				}
				seg = seg->next;
				seg_off = 0;
			}
			if (remaining_payload || tso_payload_desc == 0) {
				txq->errors++;
				txq->tso_rejected++;
				rte_pktmbuf_free(mbuf);
				sent++;
				continue;
			}

			desc_needed = (emit_vlan_ctx ? 1 : 0) + 1 + tso_payload_desc;
		} else {
			desc_needed = nsegs + (emit_vlan_ctx ? 1 : 0);
		}

		if (nsegs == 0 || (!allow_mseg && nsegs != 1)) {
			txq->errors++;
			break;
		}
		if (desc_needed > avail)
			break;

		if (tso &&
		    (!txq->tso_mss_valid || txq->tso_mss != (uint16_t)mbuf->tso_segsz)) {
			ctrl = xgmac_rd(dev, XGMAC_DMA_CH_CONTROL(txq->queue_id));
			ctrl &= ~XGMAC_MSS;
			ctrl |= XGMAC_FIELD_PREP(XGMAC_MSS, mbuf->tso_segsz);
			xgmac_wr(dev, XGMAC_DMA_CH_CONTROL(txq->queue_id), ctrl);
			txq->tso_mss = (uint16_t)mbuf->tso_segsz;
			txq->tso_mss_valid = 1;
		}

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
			desc_filled++;
		}

		if (tso) {
			tso_filled = xgmac_tx_fill_tso_descs(txq, mbuf, nb_desc, &idx, vlan_insert,
							     tso_tunnel, csum_cic, hdr_len,
							     payload_len);
			if (unlikely(tso_filled < 0)) {
				txq->errors++;
				txq->tso_rejected++;
				break;
			}
			desc_filled += (uint16_t)tso_filled;
		} else {
			bool tunnel = dev->hw_feat.tunnel &&
				      !!(mbuf->ol_flags & RTE_MBUF_F_TX_TUNNEL_MASK);

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
					if (tunnel)
						seg_tdes3 |= XGMAC_FIELD_PREP(XGMAC_TDES3_VNP,
								XGMAC_TDES3_VNP_TUNNEL);
				}
				if (j == (nsegs - 1))
					seg_tdes3 |= XGMAC_TDES3_LD;
				desc->read.tdes3 = seg_tdes3;

				txq->sw_ring[idx] = (j == 0) ? mbuf : NULL;
				idx = DESC_OFF_ADD(idx, 1, nb_desc);
				desc_filled++;
				seg = seg->next;
			}
		}

		rte_wmb();
		idx = cur & (nb_desc - 1);
		for (j = 0; j < desc_filled; j++) {
			desc = &txq->desc[idx];
			tdes3 = desc->read.tdes3;
			desc->read.tdes3 = tdes3 | XGMAC_TDES3_OWN;
			idx = DESC_OFF_ADD(idx, 1, nb_desc);
		}

		txq->nb_bytes += mbuf->pkt_len;
		cur = DESC_OFF_ADD(cur, desc_filled, nb_desc);
		avail -= desc_filled;
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
