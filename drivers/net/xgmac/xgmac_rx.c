/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <rte_mbuf.h>

#include "xgmac_ethdev.h"
#include "xgmac_regs.h"
#include "xgmac_rxtx.h"

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

	/* Reset queue state. */
	rxq->cur = 0;
	rxq->dirty = 0;
}

static inline void
xgmac_rx_refill(struct xgmac_rx_queue *rxq)
{
	struct rte_mbuf *alloc_mbufs[XGMAC_DEFAULT_RX_FREE_THRESH];
	const uint16_t thresh = rxq->rx_free_thresh;
	const uint16_t mask = rxq->nb_desc - 1;
	volatile union xgmac_rx_desc *desc;
	uint16_t start, i, tail_idx;

	while ((uint16_t)(rxq->cur - rxq->dirty) >= thresh) {
		start = rxq->dirty & mask;

		if (rte_mbuf_raw_alloc_bulk(rxq->mb_pool, alloc_mbufs, thresh) != 0) {
			rxq->rx_mbuf_alloc_failed += thresh;
			return;
		}

		for (i = 0; i < thresh; i++) {
			uint16_t idx = (start + i) & mask;

			rxq->sw_ring[idx] = alloc_mbufs[i];
			desc = &rxq->desc[idx];
			desc->read.baddr = rte_mbuf_data_iova_default(alloc_mbufs[i]);
			desc->read.rdes2 = 0;
			desc->read.rdes3 = XGMAC_RDES3_OWN;
		}

		/* Force all descriptor writes to be visible before the tail pointer update. */
		rte_wmb();

		tail_idx = (start + thresh - 1) & mask;
		xgmac_wr(rxq->dev, XGMAC_DMA_CH_RxDESC_TAIL_LPTR(rxq->queue_id),
			 xgmac_low32(rxq->ring_phys_addr + tail_idx * sizeof(union xgmac_rx_desc)));

		rxq->dirty += thresh;
	}
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

		if (unlikely(rdes3 & XGMAC_RDES3_CTXT)) {
			rte_pktmbuf_free(rxq->sw_ring[idx]);
			rxq->sw_ring[idx] = NULL;
			continue;
		}

		if (unlikely(rdes3 & XGMAC_RDES3_ES)) {
			if (flags & XGMAC_RX_SCATTER_F) {
				if (first_seg) {
					rte_pktmbuf_free(first_seg);
					first_seg = NULL;
					last_seg = NULL;
				}
			}
			rte_pktmbuf_free(rxq->sw_ring[idx]);
			errors++;
			continue;
		}

		mbuf = rxq->sw_ring[idx];
		mbuf->ol_flags = 0;

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
			nb_bytes += pkt_len;

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
			nb_bytes += pkt_len;
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

	eth_dev->data->scattered_rx = !!(f & XGMAC_RX_SCATTER_F);
	eth_dev->rx_pkt_burst = rx_burst[f];
}
