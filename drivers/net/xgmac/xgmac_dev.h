/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#ifndef __XGMAC_DEV_H__
#define __XGMAC_DEV_H__

#include <stdbool.h>

#include <rte_io.h>

struct xgmac_dev;
struct xgmac_rx_queue;
struct xgmac_tx_queue;

#define XGMAC_FRP_ENTRY_WORDS 4
#define XGMAC_FRP_WORD_BYTES  4U  /* bytes per FRP match word (single 32-bit compare slot) */

struct xgmac_frp_hw_entry {
	uint32_t data[XGMAC_FRP_ENTRY_WORDS];
};

struct xgmac_frp_action {
	uint8_t ok_index;
	uint16_t dma_ch_mask;
	bool accept_frame;
	bool reject_frame;
	bool inverse_match;
	bool next_control;
};

struct xgmac_frp_entry_cfg {
	uint32_t match_data;
	uint32_t match_en;
	uint8_t frame_offset;
	struct xgmac_frp_action action;
};

struct xgmac_frp_flow_rule {
	uint16_t byte_offset;
	const uint8_t *match_value;
	const uint8_t *match_mask;
	uint8_t match_size;
	/* Action applied to the last instruction of this rule. */
	struct xgmac_frp_action action;
};

/* MAC core helpers. */
void xgmac_mac_init(struct xgmac_dev *dev, uint16_t nb_rx_queues);
void xgmac_mac_addr_read(struct xgmac_dev *dev, uint32_t index, struct rte_ether_addr *addr);
void xgmac_mac_addr_write(struct xgmac_dev *dev, uint32_t index, const struct rte_ether_addr *addr);
void xgmac_mac_addr_clear(struct xgmac_dev *dev, uint32_t index);
void xgmac_mac_promiscuous_set(struct xgmac_dev *dev, bool enable);
void xgmac_mac_allmulticast_set(struct xgmac_dev *dev, bool enable);
void xgmac_mac_mtu_set(struct xgmac_dev *dev, uint16_t mtu);
void xgmac_vlan_insert_cfg(struct xgmac_dev *dev);
void xgmac_vlan_strip_cfg(struct xgmac_dev *dev);
int xgmac_mc_hash_filter_set(struct xgmac_dev *dev, struct rte_ether_addr *mc_addr_set,
			     uint32_t nb_mc_addr);
void xgmac_hw_features_get(struct xgmac_dev *dev);

/* MTL block helpers. */
void xgmac_mtl_init(struct xgmac_dev *dev, uint16_t nb_tx_queues, uint16_t nb_rx_queues);

/* DCB Rx helpers. */
void xgmac_dcb_configure(struct xgmac_dev *dev);

/* DMA block helpers */
int xgmac_dma_init(struct xgmac_dev *dev);
void xgmac_dma_stop(struct xgmac_dev *dev);

int xgmac_txq_prepare_stop(struct xgmac_dev *dev, uint16_t q);

/* Per-queue bring-up / tear-down. */
int xgmac_rxq_start(struct xgmac_dev *dev, struct xgmac_rx_queue *rxq);
void xgmac_rxq_stop(struct xgmac_dev *dev, struct xgmac_rx_queue *rxq);
void xgmac_txq_start(struct xgmac_dev *dev, struct xgmac_tx_queue *txq);
void xgmac_txq_stop(struct xgmac_dev *dev, struct xgmac_tx_queue *txq);

/* MMC (RMON) statistics helpers. */
void xgmac_mmc_init(struct xgmac_dev *dev);
void xgmac_mmc_stats_read(struct xgmac_dev *dev);
int xgmac_frp_stats_read(struct xgmac_dev *dev);

/* Timestamp helpers. */
int xgmac_wait_tstamp_control(struct xgmac_dev *dev, uint32_t bit);
int xgmac_timestamp_hw_init(struct xgmac_dev *dev, uint32_t tsctl_flags);
int xgmac_timestamp_configure(struct xgmac_dev *dev);
void xgmac_timestamp_disable(struct xgmac_dev *dev);
int xgmac_timestamp_read_tx(struct xgmac_dev *dev, uint32_t *sec, uint32_t *nsec);
int xgmac_timestamp_adjust_time(struct xgmac_dev *dev, int64_t delta);
int xgmac_timestamp_adjust_freq(struct xgmac_dev *dev, int64_t ppm);
void xgmac_timestamp_read_time(struct xgmac_dev *dev, uint32_t *sec, uint32_t *nsec);
int xgmac_timestamp_write_time(struct xgmac_dev *dev, uint32_t sec, uint32_t nsec);

/* RSS helpers. */
int xgmac_write_rss_hash_key(struct xgmac_dev *dev);
int xgmac_write_rss_lookup_table(struct xgmac_dev *dev);
int xgmac_rss_configure(struct xgmac_dev *dev);

void xgmac_flow_ctrl_apply(struct xgmac_dev *dev, uint16_t nb_txq, uint16_t nb_rxq);
void xgmac_rx_flow_ctrl_apply(struct xgmac_dev *dev, uint16_t nb_rxq, bool enable);
void xgmac_pfc_queue_apply(struct xgmac_dev *dev, uint16_t nb_txq, uint16_t nb_rxq);

int xgmac_frp_init(struct xgmac_dev *dev);
int xgmac_frp_enable(struct xgmac_dev *dev, bool enable);
/*
 * Compile an ordered rule-set into FRP instruction table.
 *
 * Default-miss policy (the residual entry behaviour) is selected from
 * dev->flow_isolated: 0 -> ACCEPT to DMA channel 0, 1 -> DROP.
 */
int xgmac_frp_program_rules(struct xgmac_dev *dev, const struct xgmac_frp_flow_rule *rules,
			    uint16_t nb_rules);
int xgmac_frp_entry_read(struct xgmac_dev *dev, uint16_t idx, struct xgmac_frp_hw_entry *entry);
int xgmac_frp_table_flush(struct xgmac_dev *dev);

#endif /* __XGMAC_DEV_H__ */
