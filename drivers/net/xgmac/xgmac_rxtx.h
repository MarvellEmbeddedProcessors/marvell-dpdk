/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */
#ifndef __XGMAC_RXTX_H__
#define __XGMAC_RXTX_H__

#define XGMAC_MAX_RING_DESC	     4096
#define XGMAC_MIN_RING_DESC	     32
#define XGMAC_DESC_ALIGN	     128
#define XGMAC_DEFAULT_RX_FREE_THRESH 32
#define XGMAC_DEFAULT_TX_FREE_THRESH 32

struct rte_memzone;

struct xgmac_rx_desc_read {
	/* RDES0/RDES1: buffer address (SW programmed, little-endian) */
	uint64_t baddr;
	/* RDES2: payload / buffer 2 address pointer (0 = skip buffer 2) */
	uint32_t rdes2;
	/* RDES3: ownership/control in read format */
	uint32_t rdes3;
};

struct xgmac_rx_desc_writeback {
	/* RDES0..RDES3: HW write-back status format */
	uint32_t rdes0;
	uint32_t rdes1;
	uint32_t rdes2;
	uint32_t rdes3;
};

union xgmac_rx_desc {
	struct xgmac_rx_desc_read read;
	struct xgmac_rx_desc_writeback write;
	uint32_t dword[4];
};

struct xgmac_dev;
struct rte_eth_dev;

struct xgmac_rx_queue {
	volatile union xgmac_rx_desc *desc;
	struct rte_mbuf **sw_ring;
	uint64_t cur;
	uint64_t dirty;
	uint64_t nb_bytes;
	uint16_t nb_desc;
	uint16_t port_id;
	uint16_t crc_adj;
	uint16_t buf_size;
	uint16_t rx_free_thresh;
	uint16_t queue_id;
	uint16_t pad0;
	int16_t ts_offset;
	uint64_t ts_flag;
	uint64_t nb_pkts;

	/* Per-burst refill, feature paths, stats. */
	struct xgmac_dev *dev;
	struct rte_mempool *mb_pool;
	uint64_t ring_phys_addr;
	uint64_t errors;
	uint64_t rx_mbuf_alloc_failed;
	struct rte_mbuf *pkt_first_seg;
	struct rte_mbuf *pkt_last_seg;
	struct rte_mbuf *pkt_awaiting_ts;

	/* setup-only fields. */
	const struct rte_memzone *mz;
	uint64_t offloads;
	uint8_t deferred_start;
};

struct xgmac_tx_desc_read {
	/* TDES0/TDES1: buffer address (SW programmed, little-endian) */
	uint64_t baddr;
	/* TDES2: buffer lengths and per-packet controls */
	uint32_t tdes2;
	/* TDES3: ownership/control/status */
	uint32_t tdes3;
};

struct xgmac_tx_desc_writeback {
	/* TDES0..TDES3 write-back/status view */
	uint32_t tdes0;
	uint32_t tdes1;
	uint32_t tdes2;
	uint32_t tdes3;
};

union xgmac_tx_desc {
	struct xgmac_tx_desc_read read;
	struct xgmac_tx_desc_writeback write;
	uint32_t dword[4];
};

struct xgmac_tx_queue {
	volatile union xgmac_tx_desc *desc;
	struct rte_mbuf **sw_ring;
	uint64_t cur;
	uint64_t dirty;
	uint64_t nb_pkts;
	uint64_t nb_bytes;
	uint16_t nb_desc;
	uint16_t queue_id;
	uint16_t port_id;
	uint16_t free_thresh;
	uint16_t vlan_ctx_outer_tci;
	uint16_t vlan_ctx_inner_tci;
	uint8_t vlan_ctx_qinq;
	uint8_t vlan_ctx_valid;

	uint64_t errors;

	/* setup-only fields. */
	uint64_t ring_phys_addr;
	const struct rte_memzone *mz;
	uint64_t offloads;
	uint8_t deferred_start;
};

/* XGMAC TX fastpath mode combinations */
#define XGMAC_TX_F_NONE XGMAC_TX_OFFLOAD_NONE
#define XGMAC_TX_F_CSUM XGMAC_TX_OFFLOAD_CKSUM
#define XGMAC_TX_F_VLAN XGMAC_TX_OFFLOAD_VLAN
#define XGMAC_TX_F_TSO  XGMAC_TX_OFFLOAD_TSO
#define XGMAC_TX_F_MSEG XGMAC_TX_MULTI_SEG

#define XGMAC_TX_FASTPATH_MODES \
	E(none, XGMAC_TX_F_NONE) \
	E(csum, XGMAC_TX_F_CSUM) \
	E(vlan, XGMAC_TX_F_VLAN) \
	E(csum_vlan, (XGMAC_TX_F_CSUM | XGMAC_TX_F_VLAN)) \
	E(tso, XGMAC_TX_F_TSO) \
	E(csum_tso, (XGMAC_TX_F_CSUM | XGMAC_TX_F_TSO)) \
	E(vlan_tso, (XGMAC_TX_F_VLAN | XGMAC_TX_F_TSO)) \
	E(csum_vlan_tso, (XGMAC_TX_F_CSUM | XGMAC_TX_F_VLAN | XGMAC_TX_F_TSO)) \
	E(mseg, XGMAC_TX_F_MSEG) \
	E(csum_mseg, (XGMAC_TX_F_CSUM | XGMAC_TX_F_MSEG)) \
	E(vlan_mseg, (XGMAC_TX_F_VLAN | XGMAC_TX_F_MSEG)) \
	E(csum_vlan_mseg, (XGMAC_TX_F_CSUM | XGMAC_TX_F_VLAN | XGMAC_TX_F_MSEG)) \
	E(tso_mseg, (XGMAC_TX_F_TSO | XGMAC_TX_F_MSEG)) \
	E(csum_tso_mseg, (XGMAC_TX_F_CSUM | XGMAC_TX_F_TSO | XGMAC_TX_F_MSEG)) \
	E(vlan_tso_mseg, (XGMAC_TX_F_VLAN | XGMAC_TX_F_TSO | XGMAC_TX_F_MSEG)) \
	E(csum_vlan_tso_mseg, (XGMAC_TX_F_CSUM | XGMAC_TX_F_VLAN | XGMAC_TX_F_TSO | \
			       XGMAC_TX_F_MSEG))

#define DESC_OFF_ADD(a, b, q_sz)  ((a + b) & (q_sz - 1))
#define DESC_OFF_DIFF(a, b, q_sz) ((a - b + q_sz) & (q_sz - 1))

static __rte_always_inline uint16_t
xgmac_desc_avail(uint16_t cur, uint16_t dirty, uint16_t nb_desc)
{
	uint16_t used;

	used = DESC_OFF_DIFF(cur, dirty, nb_desc);
	return (nb_desc - 1) - used;
}

extern const eth_tx_burst_t xgmac_eth_tx_burst[XGMAC_TX_MODE_MAX];
int xgmac_tx_offload_update(struct rte_eth_dev *eth_dev);

/* RX offload flags */
#define XGMAC_RX_OFFLOAD_NONE  0
#define XGMAC_RX_SCATTER_F     RTE_BIT32(0)
#define XGMAC_RX_RSS_HASH_F    RTE_BIT32(1)
#define XGMAC_RX_TIMESTAMP_F   RTE_BIT32(2)

#define XGMAC_RX_FASTPATH_MODES                                                                    \
	R(no_offload,      XGMAC_RX_OFFLOAD_NONE)                                                 \
	R(mseg,            XGMAC_RX_SCATTER_F)                                                     \
	R(rss,             XGMAC_RX_RSS_HASH_F)                                                    \
	R(mseg_rss,        (XGMAC_RX_SCATTER_F | XGMAC_RX_RSS_HASH_F))                           \
	R(ts,              XGMAC_RX_TIMESTAMP_F)                                                   \
	R(mseg_ts,         (XGMAC_RX_SCATTER_F | XGMAC_RX_TIMESTAMP_F))                           \
	R(rss_ts,          (XGMAC_RX_RSS_HASH_F | XGMAC_RX_TIMESTAMP_F))                         \
	R(mseg_rss_ts,     (XGMAC_RX_SCATTER_F | XGMAC_RX_RSS_HASH_F |                           \
			    XGMAC_RX_TIMESTAMP_F))

#define R(name, flags)                                                                             \
	uint16_t xgmac_recv_pkts_##name(void *rx_queue, struct rte_mbuf **rx_pkts,                 \
					uint16_t nb_pkts);
XGMAC_RX_FASTPATH_MODES
#undef R

void xgmac_rx_offload_update(struct rte_eth_dev *eth_dev);
void xgmac_rxq_release_mbufs(struct xgmac_rx_queue *rxq);

#endif /* __XGMAC_RXTX_H__ */
