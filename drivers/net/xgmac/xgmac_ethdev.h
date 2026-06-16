/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#ifndef __XGMAC_ETHDEV_H__
#define __XGMAC_ETHDEV_H__

#include <sys/queue.h>

#include <ethdev_driver.h>
#include <rte_io.h>
#include <rte_log.h>
#include <rte_spinlock.h>

/* Forward declaration; full definition lives in xgmac_flow.h. */
struct xgmac_flow;
TAILQ_HEAD(xgmac_flow_list, xgmac_flow);

extern int xgmac_logtype;
#define RTE_LOGTYPE_XGMAC xgmac_logtype

#define XGMAC_LOG(level, ...) RTE_LOG_LINE_PREFIX(level, XGMAC, "%s(): ", __func__, __VA_ARGS__)

/* Base unit used to decode MAC_HW_Feature1.{TX,RX}FIFOSIZE into bytes. */
#define XGMAC_FIFO_BASE_UNIT   128U
#define XGMAC_FIFO_SIZE(_fsz)  (XGMAC_FIFO_BASE_UNIT << (_fsz))

#define XGMAC_TIMEOUT_MS 5000
#define XGMAC_TX_OFFLOAD_NONE 0x0
#define XGMAC_TX_OFFLOAD_CKSUM 0x1
#define XGMAC_TX_OFFLOAD_VLAN  0x2
#define XGMAC_TX_OFFLOAD_TSO   0x4
#define XGMAC_TX_MULTI_SEG     0x8
#define XGMAC_TX_OFFLOAD_ANY   (XGMAC_TX_OFFLOAD_CKSUM | \
				XGMAC_TX_OFFLOAD_VLAN | \
				XGMAC_TX_OFFLOAD_TSO)
#define XGMAC_TX_MODE_MAX      16

/* RSS */
#define XGMAC_RSS_HASH_KEY_SIZE	    40
#define XGMAC_RSS_MAX_TABLE_SIZE    256
#define XGMAC_RSS_LOOKUP_TABLE_TYPE 0
#define XGMAC_RSS_HASH_KEY_TYPE	    1

#define XGMAC_RSS_OFFLOAD                                                                          \
	(RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV4_UDP |          \
	 RTE_ETH_RSS_IPV6 | RTE_ETH_RSS_NONFRAG_IPV6_TCP | RTE_ETH_RSS_NONFRAG_IPV6_UDP)

/* XGMAC architectural maximum number of queues (same for Rx/Tx). */
#define XGMAC_MAX_QUEUES	8

struct xgmac_pfc_rxq_cfg {
	uint8_t  enabled;
	uint8_t  tc;
	uint16_t pause_time;
};

struct xgmac_pfc_txq_cfg {
	uint8_t  enabled;
	uint8_t  tc;
};

struct xgmac_hw_features {
	uint32_t version;
	uint16_t hash_table_size;

	/* HW_Feature0 */
	uint8_t rx_coe;
	uint8_t tx_coe;
	uint8_t ptp;
	uint8_t eee;
	uint8_t addn_mac;
	uint8_t mmc;
	uint8_t tunnel; /* VxLAN/NVGRE tunnel offload (incl. tunnel TSO) */

	/* HW_Feature1 */
	uint8_t dma_addr_width;
	uint8_t rx_fifo_size;
	uint8_t tx_fifo_size;
	uint8_t tc_cnt;
	uint8_t tso;
	uint8_t rss;
	uint8_t dcb;

	/* HW_Feature2 */
	uint8_t rx_q_cnt;
	uint8_t tx_q_cnt;
	uint8_t rx_ch_cnt;
	uint8_t tx_ch_cnt;

	/* HW_Feature3 */
	uint8_t asp;
	uint8_t dvlan;
	uint8_t nrvf;
	uint8_t frp;
	uint16_t frp_parse_buf_size;
	uint16_t frp_entry_count;
};

struct xgmac_mmc_stats {
	/* Tx counters  */
	uint64_t tx_octet_count_gb;
	uint64_t tx_frame_count_gb;
	uint64_t tx_underflow_error;
	uint64_t tx_pause_frames;
	/* Rx counters. */
	uint64_t rx_frame_count_gb;
	uint64_t rx_octet_count_gb;
	uint64_t rx_crc_error;
	uint64_t rx_runt_error;
	uint64_t rx_jabber_error;
	uint64_t rx_length_error;
	uint64_t rx_fifo_overflow;
	uint64_t rx_pause_frames;
	/* FRP indirect counters (ACC_IFR). */
	uint64_t frp_drop_cnt;
	uint64_t frp_error_cnt;
	uint64_t frp_bypass_cnt;
	/* Per-DMA-channel accept counter, indexed by channel. */
	uint64_t frp_accept_cnt[XGMAC_MAX_QUEUES];
	/* Aggregated TX counters from per-queue state. */
	uint64_t tx_tso_rejected;
};

struct xgmac_dev {
	const struct rte_platform_device *pdev;
	void *csr_base;
	struct rte_eth_dev *eth_dev;
	size_t csr_size;
	uint8_t link_down;
	uint8_t vlan_outer_svlan;
	uint8_t vlan_inner_svlan;
	uint16_t vlan_outer_tpid;
	uint16_t vlan_inner_tpid;
	uint16_t rx_buf_size;
	uint16_t tx_offload_flags;
	struct xgmac_hw_features hw_feat;
	struct xgmac_mmc_stats mmc_stats;
	uint64_t rss_hf;
	uint32_t rss_options;
	uint16_t rss_table_size;
	uint8_t rss_enable;
	uint8_t pause_autoneg;
	uint8_t tx_pause;
	uint8_t rx_pause;
	uint8_t flow_ctrl_cfg_set;
	uint8_t pfc_queue_cfg_set;
	uint16_t pause_time;
	uint32_t fc_high_water;
	uint32_t fc_low_water;
	struct xgmac_pfc_rxq_cfg pfc_rxq[XGMAC_MAX_QUEUES];
	struct xgmac_pfc_txq_cfg pfc_txq[XGMAC_MAX_QUEUES];
	uint8_t timestamp_enable;
	uint8_t timesync_enable;
	uint32_t ts_addend;
	uint32_t ts_ssinc;
	uint64_t rx_tstamp;
	uint8_t rss_key[XGMAC_RSS_HASH_KEY_SIZE];
	uint32_t rss_table[XGMAC_RSS_MAX_TABLE_SIZE];
	uint8_t dcb_enable;
	uint8_t dcb_nb_tcs;
	uint8_t dcb_tc[RTE_ETH_DCB_NUM_USER_PRIORITIES];
	uint8_t flow_isolated;
	/* rte_flow / FRP state. flow_lock serialises the create/destroy/flush
	 * fast path that re-programs the FRP instruction table; flow_next_seq
	 * is a monotonic tie-breaker so equal-priority rules keep insertion
	 * order across re-programs.
	 */
	struct xgmac_flow_list flow_list;
	rte_spinlock_t flow_lock;
	uint64_t flow_next_seq;
};

#define XGMAC_FIELD_SHIFT(_mask) \
	(__builtin_ctz((uint32_t)(_mask)))

#define XGMAC_FIELD_GET(_mask, _reg) \
	(((_reg) & (_mask)) >> XGMAC_FIELD_SHIFT(_mask))

#define XGMAC_FIELD_PREP(_mask, _val) \
	(((_val) << XGMAC_FIELD_SHIFT(_mask)) & (_mask))

static inline uint32_t xgmac_low32(uint64_t addr)
{
	return (uint32_t)(addr);
}

static inline uint32_t xgmac_high32(uint64_t addr)
{
	return (uint32_t)(addr >> 32);
}

static inline uint32_t
xgmac_rd(struct xgmac_dev *dev, uint32_t offset)
{
	return rte_read32((volatile uint8_t *)dev->csr_base + offset);
}

static inline void
xgmac_wr(struct xgmac_dev *dev, uint32_t offset, uint32_t val)
{
	rte_write32(val, (volatile uint8_t *)dev->csr_base + offset);
}

static inline uint64_t
xgmac_rd64(struct xgmac_dev *dev, uint32_t lo_offset)
{
	uint32_t hi = xgmac_rd(dev, lo_offset + 4);
	uint32_t lo = xgmac_rd(dev, lo_offset);

	return ((uint64_t)hi << 32) | lo;
}

#endif /* __XGMAC_ETHDEV_H__ */
