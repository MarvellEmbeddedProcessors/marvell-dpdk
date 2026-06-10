/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */
#ifndef __XGMAC_RXTX_H__
#define __XGMAC_RXTX_H__

#define XGMAC_MAX_RING_DESC	     4096
#define XGMAC_MIN_RING_DESC	     32
#define XGMAC_DESC_ALIGN	     128

struct rte_memzone;

struct xgmac_rx_desc_read {
	/* RDES0/RDES1: buffer address (SW programmed, little-endian) */
	uint64_t baddr;
	/* RDES2: header length and control bits */
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

struct xgmac_rx_queue {
	volatile union xgmac_rx_desc *desc;
	struct rte_mbuf **sw_ring;
	uint64_t cur;
	uint64_t dirty;
	uint16_t nb_desc;
	uint16_t port_id;
	uint16_t crc_adj;
	uint16_t buf_size;
	uint16_t rx_free_thresh;
	uint16_t queue_id;
	uint16_t pad0;

	/* Per-burst refill, feature paths, stats. */
	struct xgmac_dev *dev;
	struct rte_mempool *mb_pool;
	uint64_t ring_phys_addr;

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
	uint16_t nb_desc;
	uint16_t queue_id;
	uint16_t port_id;
	uint16_t free_thresh;

	/* setup-only fields. */
	uint64_t ring_phys_addr;
	const struct rte_memzone *mz;
	uint64_t offloads;
	uint8_t deferred_start;
};

#endif /* __XGMAC_RXTX_H__ */
