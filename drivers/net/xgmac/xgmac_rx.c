/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <rte_mbuf.h>
#include <rte_mbuf_dyn.h>
#include <rte_mbuf_ptype.h>

#include "xgmac_ethdev.h"
#include "xgmac_regs.h"
#include "xgmac_rxtx.h"

#define NSEC_PER_SEC 1000000000ULL

void
xgmac_rxq_release_mbufs(struct xgmac_rx_queue *rxq)
{
	uint32_t armed, consumed, i;
	uint16_t mask, idx;

	if (!rxq || !rxq->sw_ring)
		return;

	mask = rxq->nb_desc - 1;

	/* Free mbufs in the HW-owned window, [cur, dirty + nb_desc). */
	armed = (uint32_t)(rxq->dirty + rxq->nb_desc - rxq->cur);
	for (i = 0; i < armed; i++) {
		idx = (uint16_t)((rxq->cur + i) & mask);
		if (rxq->sw_ring[idx]) {
			rte_pktmbuf_free(rxq->sw_ring[idx]);
			rxq->sw_ring[idx] = NULL;
		}
	}

	/* mbuf pointers in [dirty, cur) are already freed in fast path, avoid double-free. */
	consumed = (uint32_t)(rxq->cur - rxq->dirty);
	for (i = 0; i < consumed; i++) {
		idx = (uint16_t)((rxq->dirty + i) & mask);
		rxq->sw_ring[idx] = NULL;
	}

	/* Free partially reassembled packet if any. */
	if (rxq->pkt_first_seg) {
		rte_pktmbuf_free(rxq->pkt_first_seg);
		rxq->pkt_first_seg = NULL;
		rxq->pkt_last_seg = NULL;
	}

	/* Free a packet held awaiting its Rx timestamp, if any. */
	if (rxq->pkt_awaiting_ts) {
		rte_pktmbuf_free(rxq->pkt_awaiting_ts);
		rxq->pkt_awaiting_ts = NULL;
	}

	/* Reset queue state. */
	rxq->cur = 0;
	rxq->dirty = 0;
}

static const alignas(RTE_CACHE_LINE_SIZE) uint32_t xgmac_l34t_to_ptype[16] = {
	[0x0] = RTE_PTYPE_L2_ETHER,
	[0x1] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_TCP,
	[0x2] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_UDP,
	[0x3] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_ICMP,
	[0x4] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_IGMP,
	[0x5] = RTE_PTYPE_L2_ETHER,
	[0x6] = RTE_PTYPE_L2_ETHER,
	[0x7] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4,
	[0x8] = RTE_PTYPE_L2_ETHER,
	[0x9] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_TCP,
	[0xA] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_UDP,
	[0xB] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_ICMP,
	[0xC] = RTE_PTYPE_L2_ETHER,
	[0xD] = RTE_PTYPE_L2_ETHER,
	[0xE] = RTE_PTYPE_L2_ETHER,
	[0xF] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6,
};

static const alignas(RTE_CACHE_LINE_SIZE) uint32_t xgmac_l34t_to_cksum[16] = {
	[0x0] = 0,
	[0x1] = RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD,
	[0x2] = RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD,
	[0x3] = RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD,
	[0x4] = RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_UNKNOWN,
	[0x5] = 0,
	[0x6] = 0,
	[0x7] = RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_UNKNOWN,
	[0x8] = 0,
	[0x9] = RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD,
	[0xA] = RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD,
	[0xB] = RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD,
	[0xC] = 0,
	[0xD] = 0,
	[0xE] = 0,
	[0xF] = RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_UNKNOWN,
};

static inline void
xgmac_rx_refill(struct xgmac_rx_queue *rxq)
{
	struct rte_mbuf *alloc_mbufs[XGMAC_RX_REFILL_CHUNK];
	const uint16_t thresh = rxq->rx_free_thresh;
	const uint16_t mask = rxq->nb_desc - 1;
	volatile union xgmac_rx_desc *desc;
	uint16_t i, tail_idx;

	while ((uint16_t)(rxq->cur - rxq->dirty) >= thresh) {
		bool alloc_failed = false;
		uint16_t filled = 0;

		while (filled < thresh) {
			uint16_t chunk = RTE_MIN((uint16_t)(thresh - filled),
						 (uint16_t)XGMAC_RX_REFILL_CHUNK);
			uint16_t start = (rxq->dirty + filled) & mask;

			if (rte_mbuf_raw_alloc_bulk(rxq->mb_pool, alloc_mbufs, chunk) != 0) {
				rxq->rx_mbuf_alloc_failed += (uint32_t)(thresh - filled);
				alloc_failed = true;
				break;
			}

			for (i = 0; i < chunk; i++) {
				struct rte_mbuf *m = alloc_mbufs[i];
				uint16_t idx = (start + i) & mask;

				m->ol_flags = 0;
				m->data_off = RTE_PKTMBUF_HEADROOM;
				m->next = NULL;
				rxq->sw_ring[idx] = m;
				desc = &rxq->desc[idx];
				desc->read.baddr = rte_mbuf_data_iova_default(m);
				desc->read.rdes2 = 0;
				desc->read.rdes3 = XGMAC_RDES3_OWN;
			}

			filled += chunk;
		}

		if (filled == 0)
			return;

		/* Force all descriptor writes to be visible before the tail pointer update. */
		rte_wmb();

		tail_idx = (rxq->dirty + filled - 1) & mask;
		xgmac_wr(rxq->dev, XGMAC_DMA_CH_RxDESC_TAIL_LPTR(rxq->queue_id),
			 xgmac_low32(rxq->ring_phys_addr + tail_idx * sizeof(union xgmac_rx_desc)));

		rxq->dirty += filled;

		if (alloc_failed)
			return;
	}
}

static __rte_always_inline bool
xgmac_rx_is_cksum_only_err(uint32_t rdes3)
{
	uint32_t et = XGMAC_FIELD_GET(XGMAC_RDES3_ETLT, rdes3);

	return et == XGMAC_RDES3_ET_IP_ERR || et == XGMAC_RDES3_ET_L4_ERR;
}

static __rte_always_inline void
xgmac_rx_vlan_parse(struct rte_mbuf *mbuf, volatile union xgmac_rx_desc *desc, uint32_t rdes3)
{
	uint32_t lt, rdes0;

	if (unlikely(rdes3 & XGMAC_RDES3_ES))
		return;

	lt = XGMAC_FIELD_GET(XGMAC_RDES3_ETLT, rdes3);
	if (lt < XGMAC_RDES3_LT_VLAN_MIN)
		return;

	rdes0 = desc->write.rdes0;
	if (lt >= XGMAC_RDES3_LT_DVLAN_MIN && lt <= XGMAC_RDES3_LT_DVLAN_MAX) {
		mbuf->vlan_tci = XGMAC_FIELD_GET(XGMAC_RDES0_IVT, rdes0);
		mbuf->vlan_tci_outer = rdes0 & XGMAC_RDES0_OVT;
		mbuf->ol_flags |= RTE_MBUF_F_RX_VLAN | RTE_MBUF_F_RX_VLAN_STRIPPED |
				  RTE_MBUF_F_RX_QINQ | RTE_MBUF_F_RX_QINQ_STRIPPED;
	} else {
		mbuf->vlan_tci = rdes0 & XGMAC_RDES0_OVT;
		mbuf->ol_flags |= RTE_MBUF_F_RX_VLAN | RTE_MBUF_F_RX_VLAN_STRIPPED;
	}
}

static __rte_always_inline uint64_t
xgmac_rx_cksum_flags(uint32_t rdes3, uint32_t l34t)
{
	uint64_t ol = xgmac_l34t_to_cksum[l34t];

	if (unlikely(rdes3 & XGMAC_RDES3_ES)) {
		uint32_t et = XGMAC_FIELD_GET(XGMAC_RDES3_ETLT, rdes3);

		if (et == XGMAC_RDES3_ET_IP_ERR)
			return RTE_MBUF_F_RX_IP_CKSUM_BAD | RTE_MBUF_F_RX_L4_CKSUM_UNKNOWN;
		if (et == XGMAC_RDES3_ET_L4_ERR)
			return (ol & ~RTE_MBUF_F_RX_L4_CKSUM_GOOD) | RTE_MBUF_F_RX_L4_CKSUM_BAD;
	}

	return ol;
}

static __rte_always_inline uint16_t
xgmac_recv_pkts(void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t nb_pkts, const uint16_t flags)
{
	struct xgmac_rx_queue *rxq = rx_queue;
	struct rte_mbuf *first_seg = rxq->pkt_first_seg;
	struct rte_mbuf *last_seg = rxq->pkt_last_seg;
	const uint16_t buf_size = rxq->buf_size;
	const uint16_t mask = rxq->nb_desc - 1;
	const uint16_t crc_adj = rxq->crc_adj;
	const uint16_t port_id = rxq->port_id;
	volatile union xgmac_rx_desc *desc;
	uint32_t rdes3, pkt_len, data_len;
	uint64_t cur = rxq->cur;
	uint16_t idx, nb_rx = 0;
	struct rte_mbuf *mbuf;
	uint64_t nb_bytes = 0;
	uint64_t errors = 0;

	while (nb_rx < nb_pkts) {
		idx = cur & mask;
		desc = &rxq->desc[idx];

		/* Prefetch the next descriptors and mbuf pointers when crossing a cache line. */
		if (((cur + 1) & 0x3) == 0) {
			rte_prefetch0(&rxq->desc[(cur + 1) & mask]);
			rte_prefetch0(&rxq->sw_ring[(cur + 1) & mask]);
		}
		rte_prefetch0(rxq->sw_ring[(cur + 1) & mask]);

		rdes3 = rte_atomic_load_explicit(
			(volatile RTE_ATOMIC(uint32_t) *)&desc->write.rdes3,
			rte_memory_order_acquire);

		if (rdes3 & XGMAC_RDES3_OWN)
			break;

		cur++;

		if ((flags & XGMAC_RX_TIMESTAMP_F) && unlikely(rdes3 & XGMAC_RDES3_CTXT)) {
			if (rxq->pkt_awaiting_ts &&
			    (rdes3 & XGMAC_RDES3_TSA) && !(rdes3 & XGMAC_RDES3_TSD)) {
				uint32_t rtsl = desc->write.rdes0;
				uint32_t rtsh = desc->write.rdes1;

				if (likely(rtsl != 0xFFFFFFFF || rtsh != 0xFFFFFFFF)) {
					*RTE_MBUF_DYNFIELD(rxq->pkt_awaiting_ts, rxq->ts_offset,
						rte_mbuf_timestamp_t *) =
						(uint64_t)rtsh * NSEC_PER_SEC + rtsl;
					rxq->pkt_awaiting_ts->ol_flags |= rxq->ts_flag;
					rte_atomic_store_explicit(&rxq->dev->rx_tstamp,
								  (uint64_t)rtsh * NSEC_PER_SEC +
									  rtsl,
								  rte_memory_order_relaxed);
				}
			}

			/* Hand the held packet to the caller now that its timestamp is applied. */
			if (rxq->pkt_awaiting_ts != NULL) {
				rx_pkts[nb_rx++] = rxq->pkt_awaiting_ts;
				rxq->pkt_awaiting_ts = NULL;
			}
			rte_pktmbuf_free(rxq->sw_ring[idx]);
			rxq->sw_ring[idx] = NULL;
			continue;
		}

		if (unlikely(rdes3 & XGMAC_RDES3_ES)) {
			if (!(flags & XGMAC_RX_CKSUM_F) || !xgmac_rx_is_cksum_only_err(rdes3)) {
				if (flags & XGMAC_RX_SCATTER_F) {
					if (first_seg) {
						rte_pktmbuf_free(first_seg);
						first_seg = NULL;
						last_seg = NULL;
					}
				}
				rte_pktmbuf_free(rxq->sw_ring[idx]);
				rxq->sw_ring[idx] = NULL;
				rxq->errors++;
				continue;
			}
		}

		mbuf = rxq->sw_ring[idx];

		if (flags & XGMAC_RX_SCATTER_F) {
			mbuf->data_len = buf_size;

			if (first_seg == NULL) {
				first_seg = mbuf;
				first_seg->nb_segs = 1;
				first_seg->pkt_len = 0;
			} else {
				last_seg->next = mbuf;
				first_seg->nb_segs++;
			}
			last_seg = mbuf;

			if (!(rdes3 & XGMAC_RDES3_LD))
				continue;

			pkt_len = (rdes3 & XGMAC_RDES3_PL) - crc_adj;
			data_len = pkt_len - (uint32_t)(first_seg->nb_segs - 1) * buf_size;
			mbuf->data_len = data_len;
			first_seg->pkt_len = pkt_len;
			first_seg->port = port_id;

			if ((flags & XGMAC_RX_RSS_HASH_F) && (rdes3 & XGMAC_RDES3_RSV)) {
				first_seg->hash.rss = desc->write.rdes1;
				first_seg->ol_flags |= RTE_MBUF_F_RX_RSS_HASH;
			}
			if (flags & XGMAC_RX_CKSUM_F) {
				uint32_t l34t = XGMAC_FIELD_GET(XGMAC_RDES3_L34T, rdes3);

				first_seg->packet_type = xgmac_l34t_to_ptype[l34t];
				first_seg->ol_flags |= xgmac_rx_cksum_flags(rdes3, l34t);
			}

			if (flags & XGMAC_RX_VLAN_STRIP_F)
				xgmac_rx_vlan_parse(first_seg, desc, rdes3);

			nb_bytes += pkt_len;

			/* Hold the packet if timestamp is expected but not yet arrived. */
			if ((flags & XGMAC_RX_TIMESTAMP_F) && (rdes3 & XGMAC_RDES3_CDA))
				rxq->pkt_awaiting_ts = first_seg;
			else
				rx_pkts[nb_rx++] = first_seg;
			first_seg = NULL;
			last_seg = NULL;
		} else {
			pkt_len = (rdes3 & XGMAC_RDES3_PL) - crc_adj;
			mbuf->pkt_len = pkt_len;
			mbuf->data_len = pkt_len;
			mbuf->port = port_id;
			mbuf->nb_segs = 1;

			if ((flags & XGMAC_RX_RSS_HASH_F) && (rdes3 & XGMAC_RDES3_RSV)) {
				mbuf->hash.rss = desc->write.rdes1;
				mbuf->ol_flags |= RTE_MBUF_F_RX_RSS_HASH;
			}

			if (flags & XGMAC_RX_CKSUM_F) {
				uint32_t l34t = XGMAC_FIELD_GET(XGMAC_RDES3_L34T, rdes3);

				mbuf->packet_type = xgmac_l34t_to_ptype[l34t];
				mbuf->ol_flags |= xgmac_rx_cksum_flags(rdes3, l34t);
			}

			if (flags & XGMAC_RX_VLAN_STRIP_F)
				xgmac_rx_vlan_parse(mbuf, desc, rdes3);

			nb_bytes += pkt_len;

			/* Hold the packet if timestamp is expected but not yet arrived. */
			if ((flags & XGMAC_RX_TIMESTAMP_F) && (rdes3 & XGMAC_RDES3_CDA))
				rxq->pkt_awaiting_ts = mbuf;
			else
				rx_pkts[nb_rx++] = mbuf;
		}
	}

	rxq->cur = cur;

	if (flags & XGMAC_RX_SCATTER_F) {
		rxq->pkt_first_seg = first_seg;
		rxq->pkt_last_seg = last_seg;
	}

	rxq->nb_bytes += nb_bytes;
	rxq->errors += errors;
	rxq->nb_pkts += nb_rx;
	xgmac_rx_refill(rxq);

	return nb_rx;
}

#define R(name, flags)                                                                             \
	uint16_t __rte_noinline __rte_hot xgmac_recv_pkts_##name(void *q, struct rte_mbuf **p,     \
								 uint16_t n)                       \
	{                                                                                          \
		return xgmac_recv_pkts(q, p, n, (flags));                                          \
	}

XGMAC_RX_FASTPATH_MODES
#undef R

void
xgmac_rx_offload_update(struct rte_eth_dev *eth_dev)
{
	static const eth_rx_burst_t rx_burst[] = {
#define R(name, flags) [flags] = xgmac_recv_pkts_##name,
		XGMAC_RX_FASTPATH_MODES
#undef R
	};

	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint16_t max_pkt_len;
	uint16_t f = 0;

	max_pkt_len = eth_dev->data->mtu + RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN;

	if ((eth_dev->data->dev_conf.rxmode.offloads & RTE_ETH_RX_OFFLOAD_SCATTER) ||
	    max_pkt_len > dev->rx_buf_size)
		f |= XGMAC_RX_SCATTER_F;

	if (dev->rss_enable)
		f |= XGMAC_RX_RSS_HASH_F;

	if (eth_dev->data->dev_conf.rxmode.offloads &
	    (RTE_ETH_RX_OFFLOAD_IPV4_CKSUM | RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
	     RTE_ETH_RX_OFFLOAD_TCP_CKSUM))
		f |= XGMAC_RX_CKSUM_F;

	if (eth_dev->data->dev_conf.rxmode.offloads &
	    (RTE_ETH_RX_OFFLOAD_VLAN_STRIP | RTE_ETH_RX_OFFLOAD_QINQ_STRIP))
		f |= XGMAC_RX_VLAN_STRIP_F;

	if (dev->timestamp_enable || dev->timesync_enable) {
		uint64_t ts_flag = 0;
		int ts_offset = 0;
		uint16_t i;

		if (rte_mbuf_dyn_rx_timestamp_register(&ts_offset, &ts_flag) != 0) {
			XGMAC_LOG(ERR, "Cannot register mbuf dynfield for Rx timestamp");
		} else {
			f |= XGMAC_RX_TIMESTAMP_F;
			for (i = 0; i < eth_dev->data->nb_rx_queues; i++) {
				struct xgmac_rx_queue *rxq = eth_dev->data->rx_queues[i];

				if (rxq) {
					rxq->ts_offset = ts_offset;
					rxq->ts_flag = ts_flag;
				}
			}
		}
	}
	eth_dev->data->scattered_rx = !!(f & XGMAC_RX_SCATTER_F);
	eth_dev->rx_pkt_burst = rx_burst[f];
	if (eth_dev->data->dev_started)
		rte_eth_fp_ops[eth_dev->data->port_id].rx_pkt_burst = eth_dev->rx_pkt_burst;
}
