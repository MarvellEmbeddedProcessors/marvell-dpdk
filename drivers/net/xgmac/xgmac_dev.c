/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <errno.h>
#include <string.h>

#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_random.h>

#include "xgmac_dev.h"
#include "xgmac_ethdev.h"
#include "xgmac_regs.h"
#include "xgmac_rxtx.h"

#define XGMAC_FLOW_CONTROL_UNIT 512U
#define XGMAC_FLOW_CONTROL_VALUE(x) \
	(((x) < 1024U) ? 0U : (((x) / XGMAC_FLOW_CONTROL_UNIT) - 2U))

void
xgmac_mac_init(struct xgmac_dev *dev, uint16_t nb_rx_queues)
{
	uint64_t rx_ol = dev->eth_dev->data->dev_conf.rxmode.offloads;
	uint32_t val;
	uint16_t i;

	/* Default packet filter: hash-or-perfect filtering. */
	xgmac_wr(dev, XGMAC_PACKET_FILTER, XGMAC_FILTER_HPF);

	/* TX_CONFIG: set JD, speed = 10G XGMII. */
	val = xgmac_rd(dev, XGMAC_TX_CONFIG);
	val &= ~XGMAC_CONFIG_SS_MASK;
	val |= XGMAC_CONFIG_JD | XGMAC_CONFIG_SS_10000;
	xgmac_wr(dev, XGMAC_TX_CONFIG, val);

	/* RX_CONFIG: strip CRC (CST + ACS) unless KEEP_CRC is requested.
	 * Enable IPC only when L3/L4 cksum offload is requested.
	 */
	val = xgmac_rd(dev, XGMAC_RX_CONFIG);

	if (rx_ol & RTE_ETH_RX_OFFLOAD_KEEP_CRC)
		val &= ~(XGMAC_CONFIG_CST | XGMAC_CONFIG_ACS);
	else
		val |= XGMAC_CONFIG_CST | XGMAC_CONFIG_ACS;

	if (dev->hw_feat.rx_coe) {
		if (rx_ol & (RTE_ETH_RX_OFFLOAD_IPV4_CKSUM | RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
			     RTE_ETH_RX_OFFLOAD_TCP_CKSUM))
			val |= XGMAC_CONFIG_IPC;
		else
			val &= ~XGMAC_CONFIG_IPC;
	}
	xgmac_wr(dev, XGMAC_RX_CONFIG, val);

	/* Enable Rx queues in RXQ_CTRL0 after all MAC config is done. */
	val = 0;
	for (i = 0; i < nb_rx_queues && i < 8; i++)
		val |= (2u << (i * 2));
	xgmac_wr(dev, XGMAC_RXQ_CTRL0, val);
	xgmac_wr(dev, XGMAC_RXQ_CTRL1, 0);
	xgmac_wr(dev, XGMAC_RXQ_CTRL2, 0);
	xgmac_wr(dev, XGMAC_RXQ_CTRL3, 0);

	XGMAC_LOG(INFO, "MAC init: rx_q=%u, filter=0x%x", nb_rx_queues,
		  xgmac_rd(dev, XGMAC_PACKET_FILTER));
}

static void
xgmac_dvlp_update(struct xgmac_dev *dev)
{
	uint64_t rx_ol = dev->eth_dev->data->dev_conf.rxmode.offloads;
	uint64_t tx_ol = dev->eth_dev->data->dev_conf.txmode.offloads;
	uint32_t val = xgmac_rd(dev, XGMAC_VLAN_TAG);

	if ((rx_ol & RTE_ETH_RX_OFFLOAD_QINQ_STRIP) || (tx_ol & RTE_ETH_TX_OFFLOAD_QINQ_INSERT))
		val |= XGMAC_VLAN_EDVLP;
	else
		val &= ~XGMAC_VLAN_EDVLP;

	xgmac_wr(dev, XGMAC_VLAN_TAG, val);
}

void
xgmac_vlan_insert_cfg(struct xgmac_dev *dev)
{
	uint32_t val;

	val = xgmac_rd(dev, XGMAC_VLAN_INCL(0));
	val |= XGMAC_VLAN_VLTI;
	val &= ~(XGMAC_VLAN_CSVL | XGMAC_VLAN_VLC);
	if (dev->vlan_outer_svlan)
		val |= XGMAC_VLAN_CSVL;
	xgmac_wr(dev, XGMAC_VLAN_INCL(0), val);

	val = xgmac_rd(dev, XGMAC_INNER_VLAN_INCL);
	val |= XGMAC_VLAN_VLTI;
	val &= ~(XGMAC_VLAN_CSVL | XGMAC_VLAN_VLC);
	if (dev->vlan_inner_svlan)
		val |= XGMAC_VLAN_CSVL;
	xgmac_wr(dev, XGMAC_INNER_VLAN_INCL, val);
	xgmac_dvlp_update(dev);
}

void
xgmac_vlan_strip_cfg(struct xgmac_dev *dev)
{
	uint64_t offloads = dev->eth_dev->data->dev_conf.rxmode.offloads;
	bool strip_outer = !!(offloads & RTE_ETH_RX_OFFLOAD_VLAN_STRIP);
	bool strip_qinq = !!(offloads & RTE_ETH_RX_OFFLOAD_QINQ_STRIP);
	uint32_t val;

	/* QinQ stripping enables outer strip. */
	if (strip_qinq)
		strip_outer = true;

	val = xgmac_rd(dev, XGMAC_VLAN_TAG);
	val &= ~(XGMAC_VLAN_EVLS | XGMAC_VLAN_EVLRXS | XGMAC_VLAN_ESVL | XGMAC_VLAN_EIVLS |
		 XGMAC_VLAN_EIVLRXS);

	if (strip_outer) {
		val |= XGMAC_FIELD_PREP(XGMAC_VLAN_EVLS, XGMAC_VLAN_EVLS_ALWAYS);
		val |= XGMAC_VLAN_EVLRXS;
		if (dev->vlan_outer_svlan)
			val |= XGMAC_VLAN_ESVL;
	}

	if (strip_qinq) {
		val |= XGMAC_FIELD_PREP(XGMAC_VLAN_EIVLS, XGMAC_VLAN_EVLS_ALWAYS);
		val |= XGMAC_VLAN_EIVLRXS;
	}

	xgmac_wr(dev, XGMAC_VLAN_TAG, val);
	xgmac_dvlp_update(dev);
}

void
xgmac_rx_flow_ctrl_apply(struct xgmac_dev *dev, uint16_t nb_rxq, bool enable)
{
	uint32_t fifo_size, fifo_per_q;
	uint32_t flow, val, rfd, rfa;
	uint16_t q;

	nb_rxq = RTE_MAX(1, nb_rxq);
	fifo_size = XGMAC_FIFO_SIZE(dev->hw_feat.rx_fifo_size);
	fifo_per_q = fifo_size / nb_rxq;

	for (q = 0; q < nb_rxq; q++) {
		val = xgmac_rd(dev, XGMAC_MTL_RXQ_OPMODE(q));

		if (!enable || fifo_per_q < XGMAC_MAX_RING_DESC) {
			val &= ~XGMAC_EHFC;
			xgmac_wr(dev, XGMAC_MTL_RXQ_OPMODE(q), val);
			continue;
		}

		/*
		 * Threshold for Deactivating (RFD) / Activating (RFA) flow
		 * control. If user supplied high/low-water via flow_ctrl_set,
		 * use those; else fall back to FIFO-per-queue based defaults.
		 */
		if (dev->fc_high_water || dev->fc_low_water) {
			rfa = XGMAC_FLOW_CONTROL_VALUE(1024U * dev->fc_high_water);
			rfd = XGMAC_FLOW_CONTROL_VALUE(1024U * dev->fc_low_water);
		} else if (fifo_per_q == XGMAC_MAX_RING_DESC) {
			rfd = 0x03; /* Full - 2.5K */
			rfa = 0x01; /* Full - 1.5K */
		} else {
			rfd = 0x07; /* Full - 4.5K */
			rfa = 0x04; /* Full - 3K */
		}

		flow = xgmac_rd(dev, XGMAC_MTL_RXQ_FLOW_CONTROL(q));
		flow &= ~(XGMAC_RFD | XGMAC_RFA);
		flow |= XGMAC_FIELD_PREP(XGMAC_RFD, rfd);
		flow |= XGMAC_FIELD_PREP(XGMAC_RFA, rfa);
		xgmac_wr(dev, XGMAC_MTL_RXQ_FLOW_CONTROL(q), flow);

		val |= XGMAC_EHFC;
		xgmac_wr(dev, XGMAC_MTL_RXQ_OPMODE(q), val);
	}
}

void
xgmac_flow_ctrl_apply(struct xgmac_dev *dev, uint16_t nb_txq, uint16_t nb_rxq)
{
	uint32_t val;
	uint16_t q;

	if (!dev->flow_ctrl_cfg_set)
		return;

	for (q = 0; q < nb_txq; q++) {
		val = xgmac_rd(dev, XGMAC_Qx_TX_FLOW_CTRL(q));
		val &= ~(XGMAC_PT | XGMAC_TFE);
		if (dev->tx_pause) {
			val |= XGMAC_FIELD_PREP(XGMAC_PT, dev->pause_time);
			val |= XGMAC_TFE;
		}
		xgmac_wr(dev, XGMAC_Qx_TX_FLOW_CTRL(q), val);
	}

	val = xgmac_rd(dev, XGMAC_RX_FLOW_CTRL);
	/* Link PAUSE and PFC are mutually exclusive; make sure PFCE is off. */
	val &= ~XGMAC_PFCE;
	if (dev->rx_pause)
		val |= XGMAC_RFE;
	else
		val &= ~XGMAC_RFE;
	xgmac_wr(dev, XGMAC_RX_FLOW_CTRL, val);

	xgmac_rx_flow_ctrl_apply(dev, nb_rxq, dev->rx_pause);
}

void
xgmac_pfc_queue_apply(struct xgmac_dev *dev, uint16_t nb_txq, uint16_t nb_rxq)
{
	bool any_rx_enabled = false;
	bool any_tx_enabled = false;
	uint32_t reg, val;
	uint16_t q;
	uint8_t tc;

	if (!dev->pfc_queue_cfg_set)
		return;

	for (q = 0; q < nb_rxq && q < dev->hw_feat.rx_q_cnt; q++) {
		val = xgmac_rd(dev, XGMAC_MTL_RXQ_OPMODE(q));
		if (!dev->pfc_rxq[q].enabled) {
			val &= ~XGMAC_EHFC;
			xgmac_wr(dev, XGMAC_MTL_RXQ_OPMODE(q), val);
			continue;
		}
		any_rx_enabled = true;
		tc = dev->pfc_rxq[q].tc;

		val |= XGMAC_EHFC;
		xgmac_wr(dev, XGMAC_MTL_RXQ_OPMODE(q), val);

		if (dev->fc_high_water || dev->fc_low_water) {
			val = xgmac_rd(dev, XGMAC_MTL_RXQ_FLOW_CONTROL(q));
			val &= ~(XGMAC_RFA | XGMAC_RFD);
			val |= XGMAC_FIELD_PREP(XGMAC_RFA,
				XGMAC_FLOW_CONTROL_VALUE(1024U * dev->fc_high_water));
			val |= XGMAC_FIELD_PREP(XGMAC_RFD,
				XGMAC_FLOW_CONTROL_VALUE(1024U * dev->fc_low_water));
			xgmac_wr(dev, XGMAC_MTL_RXQ_FLOW_CONTROL(q), val);
		}

		reg = (tc < 4) ? XGMAC_TC_PRTY_MAP0 : XGMAC_TC_PRTY_MAP1;
		val = xgmac_rd(dev, reg);
		val &= ~XGMAC_PSTC(tc);
		val |= XGMAC_FIELD_PREP(XGMAC_PSTC(tc), dev->pfc_rxq[q].pause_time);
		xgmac_wr(dev, reg, val);
	}

	for (q = 0; q < nb_txq && q < dev->hw_feat.tx_q_cnt; q++) {
		val = xgmac_rd(dev, XGMAC_Qx_TX_FLOW_CTRL(q));
		val &= ~(XGMAC_TFE | XGMAC_PT);
		if (!dev->pfc_txq[q].enabled) {
			xgmac_wr(dev, XGMAC_Qx_TX_FLOW_CTRL(q), val);
			continue;
		}
		any_tx_enabled = true;
		val |= XGMAC_TFE;
		val |= XGMAC_FIELD_PREP(XGMAC_PT, dev->pause_time);
		xgmac_wr(dev, XGMAC_Qx_TX_FLOW_CTRL(q), val);
	}

	/* PFCE enables both PFC reception and auto-PFC transmission. */
	val = xgmac_rd(dev, XGMAC_RX_FLOW_CTRL);
	if (any_rx_enabled || any_tx_enabled)
		val |= XGMAC_PFCE | (any_tx_enabled ? XGMAC_RFE : 0);
	else
		val &= ~(XGMAC_PFCE | XGMAC_RFE);
	xgmac_wr(dev, XGMAC_RX_FLOW_CTRL, val);
}

static void
xgmac_set_mtl_rxq_dma_mapping(struct xgmac_dev *dev, uint16_t nb_rx_queues, bool dynamic)
{
	static const uint32_t rxq_dma_map_regs[] = {
		XGMAC_MTL_RXQ_DMA_MAP0,
		XGMAC_MTL_RXQ_DMA_MAP1,
		XGMAC_MTL_RXQ_DMA_MAP2,
		XGMAC_MTL_RXQ_DMA_MAP3,
	};
	const uint16_t queues_per_reg = 4;
	uint32_t reg_val, map_val;
	uint16_t reg, pos, q;

	for (reg = 0; reg < RTE_DIM(rxq_dma_map_regs); reg++) {
		reg_val = 0;
		for (pos = 0; pos < queues_per_reg; pos++) {
			q = reg * queues_per_reg + pos;
			if (q >= nb_rx_queues)
				break;
			map_val = q;
			if (dynamic)
				map_val |= 0x80u;
			reg_val |= map_val << (pos * 8);
		}
		xgmac_wr(dev, rxq_dma_map_regs[reg], reg_val);
	}
}

void
xgmac_mtl_init(struct xgmac_dev *dev, uint16_t nb_tx_queues, uint16_t nb_rx_queues)
{
	uint32_t val, fifo_size, fifo_per_q, qs;
	uint16_t i;

	/* RxQ-to-DMA channel 1:1 mapping. */
	xgmac_set_mtl_rxq_dma_mapping(dev, nb_rx_queues, false);

	/* Tx queue configuration: store-and-forward, TQS = (fifo_per_q / 256) - 1. */
	fifo_size = XGMAC_FIFO_SIZE(dev->hw_feat.tx_fifo_size);
	fifo_per_q = fifo_size / RTE_MAX(1, nb_tx_queues);
	qs = fifo_per_q / 256;
	if (qs > 0)
		qs -= 1;

	for (i = 0; i < nb_tx_queues; i++) {
		val = XGMAC_FIELD_PREP(XGMAC_TQS, qs);
		val |= XGMAC_TSF;
		val |= XGMAC_FIELD_PREP(XGMAC_TXQEN, XGMAC_TXQEN_EN);
		xgmac_wr(dev, XGMAC_MTL_TXQ_OPMODE(i), val);
	}

	XGMAC_LOG(DEBUG, "MTL Tx: q=%u fifo_total=%uB fifo/q=%uB TQS=%u", nb_tx_queues, fifo_size,
		  fifo_per_q, qs);

	/* Rx queue configuration: store-and-forward, RQS = (fifo_per_q / 256) - 1. */
	fifo_size = XGMAC_FIFO_SIZE(dev->hw_feat.rx_fifo_size);
	fifo_per_q = fifo_size / RTE_MAX(1, nb_rx_queues);
	qs = fifo_per_q / 256;
	if (qs > 0)
		qs -= 1;

	for (i = 0; i < nb_rx_queues; i++) {
		val = XGMAC_FIELD_PREP(XGMAC_RQS, qs);
		val |= XGMAC_RSF;
		if (dev->hw_feat.rx_coe)
			val |= XGMAC_DIS_TCP_EF;
		xgmac_wr(dev, XGMAC_MTL_RXQ_OPMODE(i), val);
	}

	XGMAC_LOG(DEBUG, "MTL Rx: q=%u fifo_total=%uB fifo/q=%uB RQS=%u", nb_rx_queues, fifo_size,
		  fifo_per_q, qs);

	XGMAC_LOG(INFO, "MTL init: tx_q=%u rx_q=%u", nb_tx_queues, nb_rx_queues);
}

void
xgmac_dcb_configure(struct xgmac_dev *dev)
{
	uint8_t psrq[XGMAC_MAX_QUEUES] = {0};
	uint32_t ctrl2 = 0, ctrl3 = 0;
	unsigned int p, q;

	if (!dev->dcb_enable)
		return;

	/* Invert dcb_tc[] into per-queue PSRQ bitmaps. */
	for (p = 0; p < RTE_ETH_DCB_NUM_USER_PRIORITIES; p++)
		psrq[dev->dcb_tc[p]] |= RTE_BIT32(p);

	for (q = 0; q < 4; q++)
		ctrl2 |= (uint32_t)psrq[q] << (q * 8);
	for (q = 0; q < 4; q++)
		ctrl3 |= (uint32_t)psrq[4 + q] << (q * 8);

	xgmac_wr(dev, XGMAC_RXQ_CTRL2, ctrl2);
	xgmac_wr(dev, XGMAC_RXQ_CTRL3, ctrl3);

	XGMAC_LOG(INFO, "DCB Rx enabled: nb_tcs=%u PSRQ Ctrl2=0x%08x Ctrl3=0x%08x", dev->dcb_nb_tcs,
		  ctrl2, ctrl3);
}

void
xgmac_mmc_init(struct xgmac_dev *dev)
{
	uint32_t val;

	/* Mask all MMC interrupts. */
	xgmac_wr(dev, XGMAC_MMC_RIER, 0);
	xgmac_wr(dev, XGMAC_MMC_TIER, 0);

	/* Clear on read and avoid wrapping to zero. */
	val = xgmac_rd(dev, XGMAC_MMC_CR);
	val |= XGMAC_MMC_CR_CNTRST | XGMAC_MMC_CR_RSTONRD | XGMAC_MMC_CR_CNTSTOPRO;
	xgmac_wr(dev, XGMAC_MMC_CR, val);

	memset(&dev->mmc_stats, 0, sizeof(dev->mmc_stats));
}

void
xgmac_mmc_stats_read(struct xgmac_dev *dev)
{
	struct xgmac_mmc_stats *s = &dev->mmc_stats;

	/* Tx counters (64-bit). */
	s->tx_octet_count_gb += xgmac_rd64(dev, XGMAC_MMC_TXOCTETCOUNT_GB_LO);
	s->tx_frame_count_gb += xgmac_rd64(dev, XGMAC_MMC_TXFRAMECOUNT_GB_LO);
	s->tx_underflow_error += xgmac_rd64(dev, XGMAC_MMC_TXUNDERFLOW_ERR_LO);
	s->tx_pause_frames += xgmac_rd64(dev, XGMAC_MMC_TXPAUSEFRAMES_LO);

	/* Rx counters (64-bit). */
	s->rx_frame_count_gb += xgmac_rd64(dev, XGMAC_MMC_RXFRAMECOUNT_GB_LO);
	s->rx_octet_count_gb += xgmac_rd64(dev, XGMAC_MMC_RXOCTETCOUNT_GB_LO);
	s->rx_crc_error += xgmac_rd64(dev, XGMAC_MMC_RXCRCERROR_LO);
	s->rx_length_error += xgmac_rd64(dev, XGMAC_MMC_RXLENGTHERROR_LO);
	s->rx_fifo_overflow += xgmac_rd64(dev, XGMAC_MMC_RXFIFOOVERFLOW_LO);
	s->rx_pause_frames += xgmac_rd64(dev, XGMAC_MMC_RXPAUSEFRAMES_LO);

	/* Rx counters (32-bit). */
	s->rx_runt_error += xgmac_rd(dev, XGMAC_MMC_RXRUNTERROR);
	s->rx_jabber_error += xgmac_rd(dev, XGMAC_MMC_RXJABBERERROR);
}

void
xgmac_mac_addr_read(struct xgmac_dev *dev, uint32_t index, struct rte_ether_addr *addr)
{
	uint32_t hi, lo;

	hi = xgmac_rd(dev, XGMAC_ADDRx_HIGH(index));
	lo = xgmac_rd(dev, XGMAC_ADDRx_LOW(index));

	addr->addr_bytes[0] = lo & 0xff;
	addr->addr_bytes[1] = (lo >> 8) & 0xff;
	addr->addr_bytes[2] = (lo >> 16) & 0xff;
	addr->addr_bytes[3] = (lo >> 24) & 0xff;
	addr->addr_bytes[4] = hi & 0xff;
	addr->addr_bytes[5] = (hi >> 8) & 0xff;
}

void
xgmac_mac_addr_write(struct xgmac_dev *dev, uint32_t index, const struct rte_ether_addr *addr)
{
	uint32_t hi, lo;

	hi = ((uint32_t)addr->addr_bytes[5] << 8) | (uint32_t)addr->addr_bytes[4] | XGMAC_AE;
	lo = ((uint32_t)addr->addr_bytes[3] << 24) | ((uint32_t)addr->addr_bytes[2] << 16) |
	     ((uint32_t)addr->addr_bytes[1] << 8) | (uint32_t)addr->addr_bytes[0];

	xgmac_wr(dev, XGMAC_ADDRx_HIGH(index), hi);
	xgmac_wr(dev, XGMAC_ADDRx_LOW(index), lo);
}

void
xgmac_mac_addr_clear(struct xgmac_dev *dev, uint32_t index)
{
	xgmac_wr(dev, XGMAC_ADDRx_HIGH(index), 0);
	xgmac_wr(dev, XGMAC_ADDRx_LOW(index), 0);
}

void
xgmac_mac_promiscuous_set(struct xgmac_dev *dev, bool enable)
{
	uint32_t val;

	val = xgmac_rd(dev, XGMAC_PACKET_FILTER);
	if (enable)
		val |= XGMAC_FILTER_PR;
	else
		val &= ~XGMAC_FILTER_PR;
	xgmac_wr(dev, XGMAC_PACKET_FILTER, val);
}

void
xgmac_mac_allmulticast_set(struct xgmac_dev *dev, bool enable)
{
	uint32_t val;

	val = xgmac_rd(dev, XGMAC_PACKET_FILTER);
	if (enable)
		val |= XGMAC_FILTER_PM;
	else
		val &= ~XGMAC_FILTER_PM;
	xgmac_wr(dev, XGMAC_PACKET_FILTER, val);
}

void
xgmac_mac_mtu_set(struct xgmac_dev *dev, uint16_t mtu)
{
	uint32_t val, frame_size;

	frame_size = mtu + RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN;

	val = xgmac_rd(dev, XGMAC_RX_CONFIG);

	if (mtu > RTE_ETHER_MTU) {
		val |= XGMAC_CONFIG_JE | XGMAC_CONFIG_GPSLCE | XGMAC_CONFIG_WD;
		val |= XGMAC_FIELD_PREP(XGMAC_CONFIG_GPSL, frame_size);
	} else {
		val &= ~(XGMAC_CONFIG_JE | XGMAC_CONFIG_GPSLCE);
		val |= XGMAC_FIELD_PREP(XGMAC_CONFIG_GPSL, 0);
	}

	xgmac_wr(dev, XGMAC_RX_CONFIG, val);
}

static uint32_t
xgmac_crc32_le(const uint8_t *data, uint32_t len)
{
	uint32_t crc = ~0u;
	uint32_t i;

	while (len--) {
		crc ^= *data++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
	}
	return crc;
}

static uint32_t
xgmac_bitrev32(uint32_t x)
{
	x = ((x & 0x55555555) << 1) | ((x & 0xaaaaaaaa) >> 1);
	x = ((x & 0x33333333) << 2) | ((x & 0xcccccccc) >> 2);
	x = ((x & 0x0f0f0f0f) << 4) | ((x & 0xf0f0f0f0) >> 4);
	x = ((x & 0x00ff00ff) << 8) | ((x & 0xff00ff00) >> 8);
	return (x << 16) | (x >> 16);
}

static int
xgmac_dma_ch_wait_stopped(struct xgmac_dev *dev, uint16_t q, uint32_t stop_mask,
			  const char *name)
{
	uint64_t tmo_ms = XGMAC_TIMEOUT_MS;
	uint32_t st;

	do {
		st = xgmac_rd(dev, XGMAC_DMA_CH_STATUS(q));
		if (st & stop_mask)
			return 0;
		rte_delay_us_sleep(1000);
		tmo_ms--;
	} while (tmo_ms);

	XGMAC_LOG(WARNING, "timeout waiting %s stop: q=%u status=0x%08x",
		  name, q, st);
	return -ETIMEDOUT;
}

int
xgmac_mc_hash_filter_set(struct xgmac_dev *dev, struct rte_ether_addr *mc_addr_set,
			 uint32_t nb_mc_addr)
{
	uint32_t mc_filter[XGMAC_MAX_HASH_TABLE];
	uint32_t hash_bits, num_hash_regs;
	uint32_t val, crc, hash_idx;
	uint32_t i;

	if (dev->hw_feat.hash_table_size == 0) {
		if (nb_mc_addr > 0)
			return -ENOTSUP;
		return 0;
	}

	hash_bits = rte_log2_u32(dev->hw_feat.hash_table_size);
	num_hash_regs = dev->hw_feat.hash_table_size / 32;
	if (num_hash_regs > XGMAC_MAX_HASH_TABLE)
		num_hash_regs = XGMAC_MAX_HASH_TABLE;

	memset(mc_filter, 0, sizeof(mc_filter));

	for (i = 0; i < nb_mc_addr; i++) {
		crc = xgmac_bitrev32(
			~xgmac_crc32_le(mc_addr_set[i].addr_bytes, RTE_ETHER_ADDR_LEN));
		hash_idx = crc >> (32 - hash_bits);
		mc_filter[hash_idx >> 5] |= RTE_BIT32(hash_idx & 0x1f);
	}

	for (i = 0; i < num_hash_regs; i++)
		xgmac_wr(dev, XGMAC_HASH_TABLE(i), mc_filter[i]);

	val = xgmac_rd(dev, XGMAC_PACKET_FILTER);
	if (nb_mc_addr > 0)
		val |= XGMAC_FILTER_HMC;
	else
		val &= ~XGMAC_FILTER_HMC;
	xgmac_wr(dev, XGMAC_PACKET_FILTER, val);

	return 0;
}

#define XGMAC_DMA_OSR_LIMIT	0x3f

static inline uint32_t
xgmac_edma_ch_mask(uint16_t nb_q)
{
	if (nb_q == 0)
		nb_q = 1;
	if (nb_q >= 30)
		return RTE_GENMASK32(29, 0);

	return RTE_BIT32(nb_q) - 1;
}

int
xgmac_dma_init(struct xgmac_dev *dev)
{
	struct rte_eth_dev *eth_dev = dev->eth_dev;
	uint16_t nb_txq = eth_dev->data->nb_tx_queues;
	uint16_t nb_rxq = eth_dev->data->nb_rx_queues;
	uint32_t tx_mask, rx_mask;
	uint64_t tmo_ms;
	uint32_t val;

	val = xgmac_rd(dev, XGMAC_DMA_MODE);
	xgmac_wr(dev, XGMAC_DMA_MODE, val | XGMAC_SWR);

	tmo_ms = XGMAC_TIMEOUT_MS;
	do {
		rte_delay_us_sleep(1000);
		val = xgmac_rd(dev, XGMAC_DMA_MODE);
		tmo_ms--;
		if (!tmo_ms) {
			XGMAC_LOG(ERR, "DMA software reset timeout");
			return -EBUSY;
		}
	} while (val & XGMAC_SWR);

	val = xgmac_rd(dev, XGMAC_DMA_SYSBUS_MODE);
	val |= XGMAC_AAL;
	val |= XGMAC_EAME;
	val &= ~XGMAC_WR_OSR_LMT;
	val |= XGMAC_FIELD_PREP(XGMAC_WR_OSR_LMT, XGMAC_DMA_OSR_LIMIT);
	val &= ~XGMAC_RD_OSR_LMT;
	val |= XGMAC_FIELD_PREP(XGMAC_RD_OSR_LMT, XGMAC_DMA_OSR_LIMIT);
	val |= XGMAC_BLEN_32;
	val |= XGMAC_UNDEF;
	xgmac_wr(dev, XGMAC_DMA_SYSBUS_MODE, val);

	tx_mask = xgmac_edma_ch_mask(nb_txq);
	rx_mask = xgmac_edma_ch_mask(nb_rxq);
	xgmac_wr(dev, XGMAC_TX_EDMA_CTRL, tx_mask);
	xgmac_wr(dev, XGMAC_RX_EDMA_CTRL, rx_mask);

	XGMAC_LOG(INFO, "EDMA channel mask: tx=0x%x rx=0x%x", tx_mask, rx_mask);

	return 0;
}

static inline uint16_t
xgmac_frp_parse_buf_size_decode(uint8_t code)
{
	static const uint16_t frp_buf_tbl[4] = { 64, 128, 256, 0 };

	return frp_buf_tbl[code & 0x3];
}

static inline uint16_t
xgmac_frp_entries_decode(uint8_t code)
{
	static const uint16_t frp_ent_tbl[4] = { 64, 128, 256, 0 };

	return frp_ent_tbl[code & 0x3];
}

void
xgmac_hw_features_get(struct xgmac_dev *dev)
{
	struct xgmac_hw_features *hw_feat = &dev->hw_feat;
	uint8_t frp_pb_code, frp_es_code;
	uint32_t hw0, hw1, hw2, hw3;

	hw_feat->version = xgmac_rd(dev, XGMAC_VERSION);

	hw0 = xgmac_rd(dev, XGMAC_HW_FEATURE0);
	hw1 = xgmac_rd(dev, XGMAC_HW_FEATURE1);
	hw2 = xgmac_rd(dev, XGMAC_HW_FEATURE2);
	hw3 = xgmac_rd(dev, XGMAC_HW_FEATURE3);

	/* HW_Feature0 */
	hw_feat->rx_coe = !!(hw0 & RTE_BIT32(16));
	hw_feat->tx_coe = !!(hw0 & RTE_BIT32(14));
	hw_feat->ptp = !!(hw0 & RTE_BIT32(12));
	hw_feat->eee = !!(hw0 & RTE_BIT32(13));
	hw_feat->mmc = !!(hw0 & RTE_BIT32(8));
	hw_feat->addn_mac = (hw0 >> 18) & 0x1f;
	hw_feat->tunnel = !!(hw0 & XGMAC_HWFEAT0_VXN);

	/* HW_Feature1 */
	hw_feat->dma_addr_width = 32 + ((hw1 >> 14) & 0x3) * 8;
	hw_feat->rx_fifo_size = (hw1 >> 0) & 0x1f;
	hw_feat->tx_fifo_size = (hw1 >> 6) & 0x1f;
	hw_feat->tso = !!(hw1 & RTE_BIT32(18));
	hw_feat->rss = !!(hw1 & RTE_BIT32(20));
	hw_feat->dcb = !!(hw1 & RTE_BIT32(16));
	hw_feat->tc_cnt = ((hw1 >> 21) & 0x7) + 1;
	hw_feat->hash_table_size = (hw1 >> 24) & 0x7;

	switch (hw_feat->hash_table_size) {
	case 1:
		hw_feat->hash_table_size = 64;
		break;
	case 2:
		hw_feat->hash_table_size = 128;
		break;
	case 3:
		hw_feat->hash_table_size = 256;
		break;
	default:
		hw_feat->hash_table_size = 0;
		break;
	}

	/* HW_Feature2 -- raw values are count - 1 */
	hw_feat->rx_q_cnt = ((hw2 >> 0) & 0xf) + 1;
	hw_feat->tx_q_cnt = ((hw2 >> 6) & 0xf) + 1;
	hw_feat->rx_ch_cnt = ((hw2 >> 12) & 0xf) + 1;
	hw_feat->tx_ch_cnt = ((hw2 >> 18) & 0xf) + 1;

	/* HW_Feature3 */
	hw_feat->asp = XGMAC_FIELD_GET(XGMAC_HWFEAT3_ASP, hw3);
	hw_feat->dvlan = !!(hw3 & XGMAC_HWFEAT3_DVLAN);
	hw_feat->nrvf = XGMAC_FIELD_GET(XGMAC_HWFEAT3_NRVF, hw3);
	hw_feat->frp = !!(hw3 & XGMAC_HWFEAT3_FRPSEL);
	frp_pb_code = XGMAC_FIELD_GET(XGMAC_HWFEAT3_FRPPB, hw3);
	frp_es_code = XGMAC_FIELD_GET(XGMAC_HWFEAT3_FRPES, hw3);
	hw_feat->frp_parse_buf_size = xgmac_frp_parse_buf_size_decode(frp_pb_code);
	hw_feat->frp_entry_count = xgmac_frp_entries_decode(frp_es_code);

	XGMAC_LOG(INFO,
		  "HW features: tx_q=%u rx_q=%u tx_ch=%u rx_ch=%u "
		  "tx_fifo=%uKB rx_fifo=%uKB addrs=%u hash_tbl=%u "
		  "asp=%u dvlan=%u nrvf=%u dma_addr=%u-bit tso=%u tunnel=%u rss=%u dcb=%u frp=%u",
		  hw_feat->tx_q_cnt, hw_feat->rx_q_cnt, hw_feat->tx_ch_cnt, hw_feat->rx_ch_cnt,
		  XGMAC_FIFO_SIZE(hw_feat->tx_fifo_size) / 1024,
		  XGMAC_FIFO_SIZE(hw_feat->rx_fifo_size) / 1024,
		  hw_feat->addn_mac + 1, hw_feat->hash_table_size, hw_feat->asp, hw_feat->dvlan,
		  hw_feat->nrvf, hw_feat->dma_addr_width, hw_feat->tso, hw_feat->tunnel,
		  hw_feat->rss, hw_feat->dcb, hw_feat->frp);
}

static int
xgmac_mtl_txq_flush(struct xgmac_dev *dev, uint16_t q)
{
	uint32_t reg = XGMAC_MTL_TXQ_OPMODE(q);
	uint64_t tmo_ms;
	uint32_t val;

	val = xgmac_rd(dev, reg);
	val |= XGMAC_FTQ;
	xgmac_wr(dev, reg, val);

	tmo_ms = XGMAC_TIMEOUT_MS;
	do {
		rte_delay_us_sleep(1000);
		val = xgmac_rd(dev, reg);
		if ((val & XGMAC_FTQ) == 0)
			return 0;
		tmo_ms--;
		if (!tmo_ms)
			break;
	} while (1);

	XGMAC_LOG(WARNING, "timed out waiting for Tx queue %u flush", q);
	return -ETIMEDOUT;
}

void
xgmac_dma_stop(struct xgmac_dev *dev)
{
	struct rte_eth_dev *eth_dev = dev->eth_dev;
	uint16_t nb_rxq = RTE_MAX(1, eth_dev->data->nb_rx_queues);
	uint16_t nb_txq = RTE_MAX(1, eth_dev->data->nb_tx_queues);
	uint32_t val;
	uint16_t q;

	/* Disable MAC RX */
	val = xgmac_rd(dev, XGMAC_RX_CONFIG);
	xgmac_wr(dev, XGMAC_RX_CONFIG, val & ~XGMAC_CONFIG_RE);

	/* Prepare each active Tx queue before disabling Tx engine. */
	for (q = 0; q < nb_txq; q++) {
		if (!eth_dev->data->tx_queues[q])
			continue;

		xgmac_txq_prepare_stop(dev, q);
	}

	/* Disable MAC TX after queues have drained. */
	val = xgmac_rd(dev, XGMAC_TX_CONFIG);
	xgmac_wr(dev, XGMAC_TX_CONFIG, val & ~XGMAC_CONFIG_TE);

	for (q = 0; q < nb_rxq; q++) {
		if (!eth_dev->data->rx_queues[q])
			continue;

		val = xgmac_rd(dev, XGMAC_DMA_CH_RX_CONTROL(q));
		xgmac_wr(dev, XGMAC_DMA_CH_RX_CONTROL(q), val & ~XGMAC_RXSR);
		xgmac_dma_ch_wait_stopped(dev, q, XGMAC_RS, "Rx DMA");
	}
	for (q = 0; q < nb_txq; q++) {
		if (!eth_dev->data->tx_queues[q])
			continue;

		val = xgmac_rd(dev, XGMAC_DMA_CH_TX_CONTROL(q));
		xgmac_wr(dev, XGMAC_DMA_CH_TX_CONTROL(q), val & ~XGMAC_TXST);
		xgmac_dma_ch_wait_stopped(dev, q, XGMAC_TS, "Tx DMA");
		xgmac_mtl_txq_flush(dev, q);
	}
}

static inline void
xgmac_txq_sw_ring_reset(struct xgmac_tx_queue *txq)
{
	uint16_t i;

	if (!txq || !txq->sw_ring)
		return;

	for (i = 0; i < txq->nb_desc; i++) {
		if (txq->sw_ring[i]) {
			rte_pktmbuf_free(txq->sw_ring[i]);
			txq->sw_ring[i] = NULL;
		}
	}
}

#define XGMAC_DMA_PBL_DEFAULT  32

/* Init a single Rx queue's descriptors and program its DMA channel.
 * Sets RxPBL, RBSZ, PBLx8, INT_EN before descriptor ring.
 */
static int
xgmac_rx_desc_init_one(struct xgmac_dev *dev, struct xgmac_rx_queue *rxq)
{
	uint32_t rxpbl = XGMAC_DMA_PBL_DEFAULT;
	volatile union xgmac_rx_desc *desc;
	uint16_t q = rxq->queue_id;
	struct rte_mbuf *mbuf;
	uint32_t val, rbufsz;
	uint16_t i;

	xgmac_rxq_release_mbufs(rxq);

	val = xgmac_rd(dev, XGMAC_DMA_CH_CONTROL(q));
	val |= XGMAC_PBLx8;
	xgmac_wr(dev, XGMAC_DMA_CH_CONTROL(q), val);
	xgmac_wr(dev, XGMAC_DMA_CH_INT_EN(q), XGMAC_DMA_INT_DEFAULT_EN);

	rbufsz = rte_pktmbuf_data_room_size(rxq->mb_pool) - RTE_PKTMBUF_HEADROOM;
	rbufsz = (rbufsz + 7) & ~7; /* align to 8 bytes */
	rxq->buf_size = rbufsz;
	if (rxq->buf_size > dev->rx_buf_size)
		dev->rx_buf_size = rxq->buf_size;
	val = xgmac_rd(dev, XGMAC_DMA_CH_RX_CONTROL(q));
	val &= ~XGMAC_RxPBL;
	val |= XGMAC_FIELD_PREP(XGMAC_RxPBL, rxpbl);
	val &= ~XGMAC_RBSZ;
	val |= XGMAC_FIELD_PREP(XGMAC_RBSZ, rbufsz);
	xgmac_wr(dev, XGMAC_DMA_CH_RX_CONTROL(q), val);

	for (i = 0; i < rxq->nb_desc; i++) {
		mbuf = rte_mbuf_raw_alloc(rxq->mb_pool);
		if (!mbuf) {
			XGMAC_LOG(ERR, "rx mbuf alloc failed q=%u i=%u", q, i);
			xgmac_rxq_release_mbufs(rxq);
			return -ENOMEM;
		}
		rxq->sw_ring[i] = mbuf;
		desc = &rxq->desc[i];
		mbuf->next = NULL;
		mbuf->data_off = RTE_PKTMBUF_HEADROOM;
		mbuf->nb_segs = 1;
		mbuf->port = rxq->port_id;
		desc->read.baddr = rte_mbuf_data_iova_default(mbuf);
		desc->read.rdes2 = 0;
		desc->read.rdes3 = XGMAC_RDES3_OWN;
		rte_wmb();
	}

	xgmac_wr(dev, XGMAC_DMA_CH_RxDESC_RING_LEN(q), rxq->nb_desc - 1);
	xgmac_wr(dev, XGMAC_DMA_CH_RxDESC_HADDR(q), xgmac_high32(rxq->ring_phys_addr));
	xgmac_wr(dev, XGMAC_DMA_CH_RxDESC_LADDR(q), xgmac_low32(rxq->ring_phys_addr));
	xgmac_wr(dev, XGMAC_DMA_CH_RxDESC_TAIL_LPTR(q),
		 xgmac_low32(rxq->ring_phys_addr +
			     (rxq->nb_desc - 1) * sizeof(union xgmac_rx_desc)));
	return 0;
}

int
xgmac_txq_prepare_stop(struct xgmac_dev *dev, uint16_t q)
{
	uint64_t tmo_ms;
	uint32_t status;
	uint32_t trcsts;

	tmo_ms = XGMAC_TIMEOUT_MS;
	do {
		rte_delay_us_sleep(1000);
		status = xgmac_rd(dev, XGMAC_MTL_TXQ_DEBUG(q));
		trcsts = (status & XGMAC_MTL_TQDR_TRCSTS) >> 1;
		if (trcsts != XGMAC_MTL_TRCSTS_WRITE &&
		    (status & XGMAC_MTL_TQDR_TXQSTS) == 0)
			return 0;
		tmo_ms--;
		if (!tmo_ms)
			break;
	} while (1);

	XGMAC_LOG(WARNING, "timed out waiting for Tx queue %u to empty", q);
	return -ETIMEDOUT;
}

/* Init a single Tx queue's descriptor ring and program its DMA channel.
 * Sets TxPBL, OSP, PBLx8 before descriptor ring.
 */
static void
xgmac_tx_desc_init_one(struct xgmac_dev *dev, struct xgmac_tx_queue *txq)
{
	uint32_t txpbl = XGMAC_DMA_PBL_DEFAULT;
	uint16_t q = txq->queue_id;
	uint32_t val;

	/* Prevent mbuf leaks across restart/reinit paths. */
	xgmac_txq_sw_ring_reset(txq);

	val = xgmac_rd(dev, XGMAC_DMA_CH_CONTROL(q));
	val |= XGMAC_PBLx8;
	xgmac_wr(dev, XGMAC_DMA_CH_CONTROL(q), val);
	xgmac_wr(dev, XGMAC_DMA_CH_INT_EN(q), XGMAC_DMA_INT_DEFAULT_EN);

	val = xgmac_rd(dev, XGMAC_DMA_CH_TX_CONTROL(q));
	val &= ~XGMAC_TxPBL;
	val |= XGMAC_FIELD_PREP(XGMAC_TxPBL, txpbl);
	xgmac_wr(dev, XGMAC_DMA_CH_TX_CONTROL(q), val | XGMAC_OSP);

	txq->cur = 0;
	txq->dirty = 0;
	txq->vlan_ctx_valid = 0;
	txq->vlan_ctx_qinq = 0;
	txq->vlan_ctx_outer_tci = 0;
	txq->vlan_ctx_inner_tci = 0;

	xgmac_wr(dev, XGMAC_DMA_CH_TxDESC_RING_LEN(q), txq->nb_desc - 1);
	xgmac_wr(dev, XGMAC_DMA_CH_TxDESC_HADDR(q), xgmac_high32(txq->ring_phys_addr));
	xgmac_wr(dev, XGMAC_DMA_CH_TxDESC_LADDR(q), xgmac_low32(txq->ring_phys_addr));
}

int
xgmac_rxq_start(struct xgmac_dev *dev, struct xgmac_rx_queue *rxq)
{
	uint16_t q = rxq->queue_id;
	uint32_t val;
	int ret;

	ret = xgmac_rx_desc_init_one(dev, rxq);
	if (ret)
		return ret;

	val = xgmac_rd(dev, XGMAC_DMA_CH_RX_CONTROL(q));
	xgmac_wr(dev, XGMAC_DMA_CH_RX_CONTROL(q), val | XGMAC_RXSR);

	return 0;
}

void
xgmac_rxq_stop(struct xgmac_dev *dev, struct xgmac_rx_queue *rxq)
{
	uint16_t q = rxq->queue_id;
	uint32_t val;

	val = xgmac_rd(dev, XGMAC_DMA_CH_RX_CONTROL(q));
	xgmac_wr(dev, XGMAC_DMA_CH_RX_CONTROL(q), val & ~XGMAC_RXSR);
	xgmac_dma_ch_wait_stopped(dev, q, XGMAC_RS, "Rx DMA");

	xgmac_rxq_release_mbufs(rxq);
}

void
xgmac_txq_start(struct xgmac_dev *dev, struct xgmac_tx_queue *txq)
{
	uint16_t q = txq->queue_id;
	uint32_t val;

	xgmac_tx_desc_init_one(dev, txq);

	val = xgmac_rd(dev, XGMAC_DMA_CH_TX_CONTROL(q));
	xgmac_wr(dev, XGMAC_DMA_CH_TX_CONTROL(q), val | XGMAC_TXST);
}

void
xgmac_txq_stop(struct xgmac_dev *dev, struct xgmac_tx_queue *txq)
{
	uint16_t q = txq->queue_id;
	uint32_t val;

	xgmac_txq_prepare_stop(dev, q);

	val = xgmac_rd(dev, XGMAC_DMA_CH_TX_CONTROL(q));
	xgmac_wr(dev, XGMAC_DMA_CH_TX_CONTROL(q), val & ~XGMAC_TXST);

	xgmac_txq_sw_ring_reset(txq);
	txq->cur = 0;
	txq->dirty = 0;
}

static int
xgmac_wait_rss_idle(struct xgmac_dev *dev)
{
	unsigned int retries = 1000;

	while (retries--) {
		if (!(xgmac_rd(dev, XGMAC_RSS_ADDR) & XGMAC_RSS_ADDR_OB))
			return 0;

		rte_delay_us(100);
	}

	return -EBUSY;
}

static int
xgmac_write_rss_reg(struct xgmac_dev *dev, unsigned int type, unsigned int index, uint32_t val)
{
	if (xgmac_wait_rss_idle(dev))
		return -EBUSY;

	xgmac_wr(dev, XGMAC_RSS_DATA, val);

	xgmac_wr(dev, XGMAC_RSS_ADDR,
		 XGMAC_FIELD_PREP(XGMAC_RSS_ADDR_RSSIA, index) |
			 XGMAC_FIELD_PREP(XGMAC_RSS_ADDR_ADDRT, type) | XGMAC_RSS_ADDR_OB);

	return xgmac_wait_rss_idle(dev);
}

int
xgmac_write_rss_hash_key(struct xgmac_dev *dev)
{
	unsigned int key_regs = XGMAC_RSS_HASH_KEY_SIZE / sizeof(uint32_t);
	uint32_t *key = (uint32_t *)dev->rss_key;
	int ret;

	while (key_regs--) {
		ret = xgmac_write_rss_reg(dev, XGMAC_RSS_HASH_KEY_TYPE, key_regs, *key++);
		if (ret)
			return ret;
	}

	return 0;
}

int
xgmac_write_rss_lookup_table(struct xgmac_dev *dev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < dev->rss_table_size; i++) {
		ret = xgmac_write_rss_reg(dev, XGMAC_RSS_LOOKUP_TABLE_TYPE, i, dev->rss_table[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static uint16_t
xgmac_rss_table_size_get(struct xgmac_dev *dev)
{
	uint32_t val, rsslsz;
	uint16_t size;

	val = xgmac_rd(dev, XGMAC_RSS_CTRL);
	rsslsz = XGMAC_FIELD_GET(XGMAC_RSS_CTRL_RSSLSZ, val);
	size = RTE_MIN(16u << rsslsz, (unsigned int)XGMAC_RSS_MAX_TABLE_SIZE);

	XGMAC_LOG(INFO, "RSS RETA size: %u entries (RSSLSZ=%u)", size, rsslsz);
	return size;
}

static void
xgmac_rss_options_set(struct xgmac_dev *dev)
{
	uint64_t rss_hf = dev->rss_hf;

	dev->rss_options = 0;

	if (rss_hf & (RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_IPV6))
		dev->rss_options |= XGMAC_IP2TE;
	if (rss_hf & (RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV6_TCP))
		dev->rss_options |= XGMAC_TCP4TE;
	if (rss_hf & (RTE_ETH_RSS_NONFRAG_IPV4_UDP | RTE_ETH_RSS_NONFRAG_IPV6_UDP))
		dev->rss_options |= XGMAC_UDP4TE;
}

static int
xgmac_rss_enable(struct xgmac_dev *dev)
{
	int ret;

	ret = xgmac_write_rss_hash_key(dev);
	if (ret) {
		XGMAC_LOG(ERR, "RSS hash key write failed: %d", ret);
		return ret;
	}

	ret = xgmac_write_rss_lookup_table(dev);
	if (ret) {
		XGMAC_LOG(ERR, "RSS lookup table write failed: %d", ret);
		return ret;
	}

	xgmac_wr(dev, XGMAC_RSS_CTRL, dev->rss_options | XGMAC_RSSE);
	XGMAC_LOG(DEBUG, "RSS_CTRL after program: 0x%08x (options=0x%x)",
		  xgmac_rd(dev, XGMAC_RSS_CTRL), dev->rss_options);

	return 0;
}

static void
xgmac_rss_set_dynamic_mtl_mapping(struct xgmac_dev *dev, uint16_t nb_rx_queues)
{
	xgmac_set_mtl_rxq_dma_mapping(dev, nb_rx_queues, true);
}

/*
 * PTP reference clock frequency (Hz).
 *
 * In fine correction mode, every PTP ref clock tick the addend is added
 * to a 32-bit accumulator.  On overflow, SSINC nanoseconds are added to
 * the system time:
 *
 *   effective_ns_per_sec = (ref_clk * addend / 2^32) * SSINC
 *
 * Adjusting the addend allows fine-tuning the PTP clock frequency
 * without changing the hardware ref clock.
 */
static inline uint64_t
xgmac_ptp_ref_clk_hz(void)
{
	return 100000000ULL;
}

int
xgmac_wait_tstamp_control(struct xgmac_dev *dev, uint32_t bit)
{
	unsigned int retries = 100;

	while (retries > 0) {
		if (!(xgmac_rd(dev, XGMAC_TIMESTAMP_CONTROL) & bit))
			return 0;
		rte_delay_ms(1);
		retries--;
	}

	XGMAC_LOG(ERR, "Timed out waiting for TSCTL bit 0x%x to clear", bit);
	return -ETIMEDOUT;
}

int
xgmac_timestamp_hw_init(struct xgmac_dev *dev, uint32_t tsctl_flags)
{
	uint64_t ptp_ref_clk = xgmac_ptp_ref_clk_hz();
	uint32_t ssinc, addend, val;
	uint64_t target_freq;
	int ret;

	xgmac_wr(dev, XGMAC_TIMESTAMP_CONTROL, tsctl_flags);

	/*
	 * Target the overflow rate at ref_clk/2 so the addend sits at 50% of
	 * the 32-bit range, leaving headroom for frequency adjustment in both
	 * directions.
	 */
	target_freq = ptp_ref_clk / 2;
	ssinc = (uint32_t)(1000000000ULL / target_freq);
	addend = (uint32_t)((target_freq << 32) / ptp_ref_clk);

	dev->ts_ssinc = ssinc;
	dev->ts_addend = addend;

	xgmac_wr(dev, XGMAC_SUB_SECOND_INCR,
		  XGMAC_FIELD_PREP(XGMAC_SSINC_SSINC, ssinc));
	xgmac_wr(dev, XGMAC_TIMESTAMP_ADDEND, addend);

	val = xgmac_rd(dev, XGMAC_TIMESTAMP_CONTROL);
	xgmac_wr(dev, XGMAC_TIMESTAMP_CONTROL, val | XGMAC_TSTAMP_TSADDREG);
	ret = xgmac_wait_tstamp_control(dev, XGMAC_TSTAMP_TSADDREG);
	if (ret)
		return ret;

	xgmac_wr(dev, XGMAC_SYSTEM_TIME_SEC_UPD, 0);
	xgmac_wr(dev, XGMAC_SYSTEM_TIME_NSEC_UPD, 0);

	val = xgmac_rd(dev, XGMAC_TIMESTAMP_CONTROL);
	xgmac_wr(dev, XGMAC_TIMESTAMP_CONTROL, val | XGMAC_TSTAMP_TSINIT);
	ret = xgmac_wait_tstamp_control(dev, XGMAC_TSTAMP_TSINIT);
	if (ret)
		return ret;

	XGMAC_LOG(DEBUG, "TS hw init: TSCTL=0x%08x ssinc=%u addend=0x%08x ref_clk=%lluHz",
		  xgmac_rd(dev, XGMAC_TIMESTAMP_CONTROL), ssinc, addend,
		  (unsigned long long)ptp_ref_clk);
	return 0;
}

static uint32_t
xgmac_timestamp_ctrl_flags(struct xgmac_dev *dev)
{
	uint32_t val;

	if (!dev->timestamp_enable && !dev->timesync_enable)
		return 0;

	val = XGMAC_TSTAMP_TSENA
	    | XGMAC_TSTAMP_TSCFUPDT
	    | XGMAC_TSTAMP_TSCTRLSSR
	    | XGMAC_TSTAMP_TSVER2ENA
	    | XGMAC_TSTAMP_TXTSSTSM;

	if (dev->timestamp_enable) {
		val |= XGMAC_TSTAMP_TSENALL;
	} else {
		val |= XGMAC_TSTAMP_TSIPENA
		     | XGMAC_TSTAMP_TSIPV6ENA
		     | XGMAC_TSTAMP_TSIPV4ENA
		     | XGMAC_TSTAMP_TSEVNTENA
		     | XGMAC_FIELD_PREP(XGMAC_TSTAMP_SNAPTYPSEL, 2);
	}

	return val;
}

int
xgmac_timestamp_configure(struct xgmac_dev *dev)
{
	uint32_t flags;

	flags = xgmac_timestamp_ctrl_flags(dev);
	if (!flags)
		return 0;

	return xgmac_timestamp_hw_init(dev, flags);
}

void
xgmac_timestamp_disable(struct xgmac_dev *dev)
{
	uint32_t val;

	val = xgmac_rd(dev, XGMAC_TIMESTAMP_CONTROL);
	xgmac_wr(dev, XGMAC_TIMESTAMP_CONTROL, val & ~XGMAC_TSTAMP_TSENA);
}

int
xgmac_timestamp_read_tx(struct xgmac_dev *dev, uint32_t *sec, uint32_t *nsec)
{
	uint32_t status;

	status = xgmac_rd(dev, XGMAC_TIMESTAMP_STATUS);

	/* Check if a valid timestamp is available. */
	if (!(status & XGMAC_TSSTATUS_TXTSC))
		return -EAGAIN;

	*nsec = xgmac_rd(dev, XGMAC_TXTIMESTAMP_NSEC);
	*sec = xgmac_rd(dev, XGMAC_TXTIMESTAMP_SEC);

	/* Check if the timestamp was overwritten before it was read. */
	if (*nsec & XGMAC_TXTSSTSMIS)
		return -EINVAL;

	return 0;
}

#define NSEC_PER_SEC 1000000000ULL

int
xgmac_timestamp_adjust_time(struct xgmac_dev *dev, int64_t delta)
{
	uint32_t sec, nsec, val;
	bool neg = false;
	uint64_t mag;

	if (delta < 0) {
		neg = true;
		mag = -(uint64_t)delta;
	} else {
		mag = (uint64_t)delta;
	}

	sec = (uint32_t)(mag / NSEC_PER_SEC);
	nsec = (uint32_t)(mag % NSEC_PER_SEC);

	if (neg) {
		if (sec)
			sec = 0xFFFFFFFF - (sec - 1);
		if (nsec)
			nsec = NSEC_PER_SEC - nsec;
	}

	xgmac_wr(dev, XGMAC_SYSTEM_TIME_SEC_UPD, sec);
	xgmac_wr(dev, XGMAC_SYSTEM_TIME_NSEC_UPD, neg ? (nsec | XGMAC_NSEC_UPD_ADDSUB) : nsec);

	val = xgmac_rd(dev, XGMAC_TIMESTAMP_CONTROL);
	xgmac_wr(dev, XGMAC_TIMESTAMP_CONTROL, val | XGMAC_TSTAMP_TSUPDT);
	return xgmac_wait_tstamp_control(dev, XGMAC_TSTAMP_TSUPDT);
}

int
xgmac_timestamp_adjust_freq(struct xgmac_dev *dev, int64_t ppm)
{
	uint64_t abs_ppm, diff;
	uint32_t addend, val;
	bool neg = false;

	if (ppm < 0) {
		neg = true;
		abs_ppm = (uint64_t)(-ppm);
	} else {
		abs_ppm = (uint64_t)ppm;
	}

	/* ppm is scaled PPM (16.16 fixed-point representation) */
	diff = (uint64_t)dev->ts_addend * abs_ppm;
	diff >>= 16;
	diff /= 1000000ULL;

	addend = neg ? dev->ts_addend - (uint32_t)diff : dev->ts_addend + (uint32_t)diff;

	xgmac_wr(dev, XGMAC_TIMESTAMP_ADDEND, addend);

	val = xgmac_rd(dev, XGMAC_TIMESTAMP_CONTROL);
	xgmac_wr(dev, XGMAC_TIMESTAMP_CONTROL, val | XGMAC_TSTAMP_TSADDREG);
	return xgmac_wait_tstamp_control(dev, XGMAC_TSTAMP_TSADDREG);
}

void
xgmac_timestamp_read_time(struct xgmac_dev *dev, uint32_t *sec, uint32_t *nsec)
{
	*sec = xgmac_rd(dev, XGMAC_SYSTEM_TIME_SEC);
	*nsec = xgmac_rd(dev, XGMAC_SYSTEM_TIME_NSEC);
}

int
xgmac_timestamp_write_time(struct xgmac_dev *dev, uint32_t sec, uint32_t nsec)
{
	uint32_t val;

	xgmac_wr(dev, XGMAC_SYSTEM_TIME_SEC_UPD, sec);
	xgmac_wr(dev, XGMAC_SYSTEM_TIME_NSEC_UPD, nsec);

	val = xgmac_rd(dev, XGMAC_TIMESTAMP_CONTROL);
	xgmac_wr(dev, XGMAC_TIMESTAMP_CONTROL, val | XGMAC_TSTAMP_TSINIT);
	return xgmac_wait_tstamp_control(dev, XGMAC_TSTAMP_TSINIT);
}

int
xgmac_rss_configure(struct xgmac_dev *dev)
{
	struct rte_eth_dev *eth_dev = dev->eth_dev;
	uint16_t nb_rx_queues = eth_dev->data->nb_rx_queues;
	struct rte_eth_rss_conf *rss_conf;
	uint32_t i;
	int ret;

	if (!dev->rss_enable) {
		uint32_t val = xgmac_rd(dev, XGMAC_RSS_CTRL);

		xgmac_wr(dev, XGMAC_RSS_CTRL, val & ~XGMAC_RSSE);
		return 0;
	}

	if (!dev->rss_table_size)
		dev->rss_table_size = xgmac_rss_table_size_get(dev);

	rss_conf = &eth_dev->data->dev_conf.rx_adv_conf.rss_conf;

	if (rss_conf->rss_key != NULL && rss_conf->rss_key_len == XGMAC_RSS_HASH_KEY_SIZE) {
		memcpy(dev->rss_key, rss_conf->rss_key, XGMAC_RSS_HASH_KEY_SIZE);
	} else {
		uint32_t *key = (uint32_t *)dev->rss_key;

		for (i = 0; i < XGMAC_RSS_HASH_KEY_SIZE / sizeof(uint32_t); i++)
			key[i] = (uint32_t)rte_rand();
	}

	for (i = 0; i < dev->rss_table_size; i++)
		dev->rss_table[i] = i % nb_rx_queues;

	if (rss_conf->rss_hf)
		dev->rss_hf = rss_conf->rss_hf & XGMAC_RSS_OFFLOAD;
	else
		dev->rss_hf = XGMAC_RSS_OFFLOAD;

	xgmac_rss_options_set(dev);

	ret = xgmac_rss_enable(dev);
	if (ret) {
		XGMAC_LOG(ERR, "Failed to enable RSS");
		return ret;
	}

	xgmac_rss_set_dynamic_mtl_mapping(dev, nb_rx_queues);

	XGMAC_LOG(INFO, "RSS enabled: queues=%u reta_size=%u hf=0x%" PRIx64
		  " RSS_CTRL=0x%x MTL_MAP0=0x%x",
		  nb_rx_queues, dev->rss_table_size, dev->rss_hf,
		  xgmac_rd(dev, XGMAC_RSS_CTRL),
		  xgmac_rd(dev, XGMAC_MTL_RXQ_DMA_MAP0));
	return 0;
}

static int
xgmac_wait_frp_idle(struct xgmac_dev *dev)
{
	uint64_t tmo_ms = XGMAC_TIMEOUT_MS;
	uint32_t val;

	do {
		val = xgmac_rd(dev, XGMAC_MTL_RXP_IACC_CTRL_ST);
		if (!(val & XGMAC_STARTBUSY))
			return 0;
		rte_delay_us_sleep(1000);
		tmo_ms--;
	} while (tmo_ms);

	XGMAC_LOG(WARNING, "FRP indirect access timeout: iacc=0x%08x", val);
	return -ETIMEDOUT;
}

static int
xgmac_iacc_access(struct xgmac_dev *dev, uint8_t accsel, uint16_t addr, uint32_t *data, bool write)
{
	uint32_t ctrl;
	int ret;

	if (!data)
		return -EINVAL;
	if (accsel > XGMAC_ACCSEL_ACC_MMC)
		return -EINVAL;

	ret = xgmac_wait_frp_idle(dev);
	if (ret)
		return ret;

	if (write)
		xgmac_wr(dev, XGMAC_MTL_RXP_IACC_DATA, *data);

	ctrl = XGMAC_FIELD_PREP(XGMAC_ACCSEL, accsel) |
	       XGMAC_FIELD_PREP(XGMAC_RXP_IACC_ADDR, addr);
	if (write)
		ctrl |= XGMAC_WRRDN;
	ctrl |= XGMAC_STARTBUSY;

	xgmac_wr(dev, XGMAC_MTL_RXP_IACC_CTRL_ST, ctrl);

	ret = xgmac_wait_frp_idle(dev);
	if (ret)
		return ret;

	if (!write)
		*data = xgmac_rd(dev, XGMAC_MTL_RXP_IACC_DATA);

	return 0;
}

static int
xgmac_wait_frp_state(struct xgmac_dev *dev, bool active)
{
	uint64_t tmo_ms = XGMAC_TIMEOUT_MS;
	uint32_t val;

	do {
		val = xgmac_rd(dev, XGMAC_MTL_RXP_CONTROL_STATUS);
		if (!!(val & XGMAC_RXPI) == active)
			return 0;
		rte_delay_us_sleep(1000);
		tmo_ms--;
	} while (tmo_ms);

	return -ETIMEDOUT;
}

int
xgmac_frp_enable(struct xgmac_dev *dev, bool enable)
{
	uint16_t nb_rx_queues;
	uint32_t val;

	if (!dev)
		return -EINVAL;
	if (!dev->hw_feat.frp)
		return -ENOTSUP;

	nb_rx_queues = RTE_MAX(1, dev->eth_dev->data->nb_rx_queues);
	if (enable)
		xgmac_set_mtl_rxq_dma_mapping(dev, nb_rx_queues, true);

	val = xgmac_rd(dev, XGMAC_MTL_OPMODE);
	if (enable)
		val |= XGMAC_FRPE;
	else
		val &= ~XGMAC_FRPE;
	xgmac_wr(dev, XGMAC_MTL_OPMODE, val);
	if (xgmac_wait_frp_state(dev, enable))
		XGMAC_LOG(WARNING, "timeout waiting FRP state=%u (RXPI)", enable);

	if (!enable)
		xgmac_set_mtl_rxq_dma_mapping(dev, nb_rx_queues, false);

	return 0;
}

/* Validate a cfg against HW capabilities. */
static int
xgmac_frp_cfg_validate(struct xgmac_dev *dev, const struct xgmac_frp_entry_cfg *cfg)
{
	uint16_t entry_count;
	uint16_t parse_buf_size;
	uint16_t valid_ch_mask;
	uint8_t max_frame_offset;

	parse_buf_size = dev->hw_feat.frp_parse_buf_size;
	if (!parse_buf_size)
		return -EINVAL;
	max_frame_offset = (parse_buf_size / sizeof(uint32_t)) - 1;
	if (cfg->frame_offset > max_frame_offset)
		return -EINVAL;
	if (dev->hw_feat.rx_ch_cnt >= 16)
		valid_ch_mask = UINT16_MAX;
	else
		valid_ch_mask = (uint16_t)(RTE_BIT32(dev->hw_feat.rx_ch_cnt) - 1);
	/* PASS (AF=1, RF=0) requires a non-zero DCH that lies within rx_ch_cnt. */
	if (cfg->action.accept_frame && !cfg->action.reject_frame &&
	    (cfg->action.dma_ch_mask == 0 || (cfg->action.dma_ch_mask & ~valid_ch_mask)))
		return -EINVAL;
	/* DROP / intermediate / BYPASS: HW ignores DCH. */
	if ((!cfg->action.accept_frame || cfg->action.reject_frame) &&
	    (cfg->action.dma_ch_mask & ~valid_ch_mask))
		return -EINVAL;
	entry_count = dev->hw_feat.frp_entry_count;
	if (cfg->action.ok_index >= entry_count)
		return -EINVAL;

	return 0;
}

/* Pack a (pre-validated) cfg into the 16-byte FRP HW instruction layout. */
static void
xgmac_frp_cfg_pack(const struct xgmac_frp_entry_cfg *cfg, struct xgmac_frp_hw_entry *entry)
{
	uint32_t ctrl = 0;

	if (cfg->action.accept_frame)
		ctrl |= RTE_BIT32(0);
	if (cfg->action.reject_frame)
		ctrl |= RTE_BIT32(1);
	if (cfg->action.inverse_match)
		ctrl |= RTE_BIT32(2);
	if (cfg->action.next_control)
		ctrl |= RTE_BIT32(3);

	entry->data[0] = cfg->match_data;
	entry->data[1] = cfg->match_en;
	entry->data[2] =
		ctrl | ((uint32_t)cfg->frame_offset << 8) | ((uint32_t)cfg->action.ok_index << 16);
	/*
	 * DCH is [111:96] => word3[15:0]. HW uses DCH only on PASS (AF=1, RF=0).
	 * BYPASS (AF=1, RF=1), DROP (AF=0, RF=1) and intermediate (AF=0, RF=0) DCH is don't-care.
	 */
	entry->data[3] = (cfg->action.accept_frame && !cfg->action.reject_frame) ?
				 (uint32_t)cfg->action.dma_ch_mask :
				 0;
}

static int
xgmac_frp_entry_write(struct xgmac_dev *dev, uint16_t idx, const struct xgmac_frp_hw_entry *entry)
{
	uint16_t entry_count;
	uint16_t base;
	uint32_t i;
	int ret;

	entry_count = dev->hw_feat.frp_entry_count;
	if (idx >= entry_count)
		return -EINVAL;

	base = idx * XGMAC_FRP_ENTRY_WORDS;
	for (i = 0; i < XGMAC_FRP_ENTRY_WORDS; i++) {
		uint32_t data = entry->data[i];

		ret = xgmac_iacc_access(dev, XGMAC_ACCSEL_ACC_IT, base + i, &data, true);
		if (ret)
			return ret;
	}

	return 0;
}

static int
xgmac_frp_map_flow_to_cfg(struct xgmac_dev *dev, uint16_t byte_offset, const uint8_t *match_value,
			  const uint8_t *match_mask, uint8_t match_size,
			  const struct xgmac_frp_action *action, uint8_t miss_ok_index,
			  struct xgmac_frp_entry_cfg *generated_cfg, uint16_t *generated_cfg_num)
{
	uint32_t end_byte = byte_offset + match_size - 1u;
	uint32_t first_word = byte_offset / XGMAC_FRP_WORD_BYTES;
	uint32_t last_word = end_byte / XGMAC_FRP_WORD_BYTES;
	struct xgmac_frp_entry_cfg cfg;
	uint16_t cfg_num = 0;
	uint32_t word, b;
	int ret;

	/* Defence in depth: a zero match_size would underflow end_byte above. */
	if (match_size == 0)
		return -EINVAL;

	if (action->inverse_match && last_word != first_word)
		return -ENOTSUP;

	for (word = first_word; word <= last_word; word++) {
		uint32_t word_start = word * XGMAC_FRP_WORD_BYTES;
		uint32_t seg_start = (word_start > byte_offset) ? word_start : byte_offset;
		uint32_t word_end = word_start + XGMAC_FRP_WORD_BYTES;
		uint32_t match_end = byte_offset + match_size;
		uint32_t seg_end = (word_end < match_end) ? word_end : match_end;

		memset(&cfg, 0, sizeof(cfg));
		cfg.frame_offset = (uint8_t)word;

		if (word == last_word) {
			/* Terminal entry: carries the rule's action. */
			cfg.action = *action;
		} else {
			/*
			 * Intermediate compare entry:
			 *  - NIC=1: on match continue sequentially.
			 *  - OKI=miss_ok_index: on mismatch jump to caller-specified
			 *    fail path (usually next rule/residual entry).
			 */
			cfg.action.next_control = true;
			cfg.action.ok_index = miss_ok_index;
		}

		for (b = seg_start; b < seg_end; b++) {
			uint32_t src = b - byte_offset;
			uint32_t pos = b - word_start;

			/* Shift casts are required: uint8_t << 24 in signed int is UB. */
			cfg.match_data |= (uint32_t)match_value[src] << (pos * 8);
			cfg.match_en   |= (uint32_t)match_mask[src]  << (pos * 8);
		}

		ret = xgmac_frp_cfg_validate(dev, &cfg);
		if (ret)
			return ret;

		generated_cfg[cfg_num++] = cfg;
	}

	*generated_cfg_num = cfg_num;
	return 0;
}

static int
xgmac_frp_compile_rules_at(struct xgmac_dev *dev, const struct xgmac_frp_flow_rule *rules,
			   uint16_t nb_rules, uint16_t base_index, uint8_t residual_ok_index,
			   struct xgmac_frp_entry_cfg *generated_cfg,
			   uint16_t generated_cfg_max, uint16_t *generated_cfg_num)
{
	uint16_t entry_count, compiled = 0;
	uint16_t i, mapped;
	int ret;

	entry_count = dev->hw_feat.frp_entry_count;
	if (base_index >= entry_count || residual_ok_index >= entry_count)
		return -EINVAL;
	if (base_index > residual_ok_index)
		return -EINVAL;

	for (i = 0; i < nb_rules; i++) {
		const struct xgmac_frp_flow_rule *rule = &rules[i];
		uint16_t first_word, last_word, req_entries;
		uint8_t miss_ok_index;
		uint16_t next_start;

		/* Skip zero-size rules (mirrors count_lanes) to keep lane indices consistent. */
		if (rule->match_size == 0)
			continue;

		first_word = rule->byte_offset / XGMAC_FRP_WORD_BYTES;
		last_word = (rule->byte_offset + rule->match_size - 1) / XGMAC_FRP_WORD_BYTES;
		req_entries = last_word - first_word + 1;

		if (compiled + req_entries > generated_cfg_max)
			return -ENOSPC;

		/*
		 * Rules must fit strictly before the residual slot, so the
		 * default-miss entry at residual_ok_index is never overwritten.
		 */
		next_start = base_index + compiled + req_entries;
		if (next_start > residual_ok_index)
			return -ENOSPC;

		miss_ok_index = (i + 1 < nb_rules) ? (uint8_t)next_start : residual_ok_index;

		ret = xgmac_frp_map_flow_to_cfg(dev, rule->byte_offset, rule->match_value,
						rule->match_mask, rule->match_size, &rule->action,
						miss_ok_index, &generated_cfg[compiled], &mapped);
		if (ret)
			return ret;
		compiled += mapped;
	}

	*generated_cfg_num = compiled;
	return 0;
}

/*
 * NVE = last valid IT index; grow forward only so already-programmed
 * entries stay reachable. NPE = max entries a frame may traverse; set it
 * to NVE so a misbehaving chain aborts with NPEOVIS instead of stalling.
 */
static void
xgmac_frp_grow_nve(struct xgmac_dev *dev, uint16_t want_idx)
{
	uint32_t val = xgmac_rd(dev, XGMAC_MTL_RXP_CONTROL_STATUS);
	uint16_t cur = (uint16_t)XGMAC_FIELD_GET(XGMAC_NVE, val);

	if (cur > want_idx)
		want_idx = cur;

	val &= ~(XGMAC_NVE | XGMAC_NPE);
	val |= XGMAC_FIELD_PREP(XGMAC_NVE, want_idx);
	val |= XGMAC_FIELD_PREP(XGMAC_NPE, want_idx);
	xgmac_wr(dev, XGMAC_MTL_RXP_CONTROL_STATUS, val);
}

static int
xgmac_frp_program_rules_at(struct xgmac_dev *dev, const struct xgmac_frp_flow_rule *rules,
			   uint16_t nb_rules, uint16_t base_index, uint8_t residual_ok_index)
{
	struct xgmac_frp_entry_cfg *cfg_list = NULL;
	struct xgmac_frp_hw_entry entry;
	uint16_t entry_count, cfg_num = 0;
	uint16_t avail_entries, i;
	int ret;

	entry_count = dev->hw_feat.frp_entry_count;
	avail_entries = entry_count - base_index;
	cfg_list = rte_zmalloc("xgmac_frp_cfg", avail_entries * sizeof(*cfg_list), 0);
	if (!cfg_list)
		return -ENOMEM;

	ret = xgmac_frp_compile_rules_at(dev, rules, nb_rules, base_index, residual_ok_index,
					 cfg_list, avail_entries, &cfg_num);
	if (ret)
		goto out;

	if (cfg_num == 0) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < cfg_num; i++) {
		/* cfg_list was already validated in compile_rules_at. */
		xgmac_frp_cfg_pack(&cfg_list[i], &entry);

		ret = xgmac_frp_entry_write(dev, base_index + i, &entry);
		if (ret)
			goto out;
	}

out:
	rte_free(cfg_list);
	return ret;
}

enum xgmac_frp_default_miss {
	XGMAC_FRP_DEFAULT_MISS_ACCEPT = 0,
	XGMAC_FRP_DEFAULT_MISS_DROP = 1,
};

static int
xgmac_frp_program_default_miss_entry(struct xgmac_dev *dev, uint8_t residual_ok_index,
				     enum xgmac_frp_default_miss policy, uint16_t dma_ch_mask)
{
	struct xgmac_frp_entry_cfg residual_cfg = { 0 };
	struct xgmac_frp_hw_entry residual_entry;
	int ret;

	residual_cfg.action.ok_index = residual_ok_index;
	switch (policy) {
	case XGMAC_FRP_DEFAULT_MISS_ACCEPT:
		residual_cfg.action.accept_frame = true;
		residual_cfg.action.dma_ch_mask = dma_ch_mask;
		break;
	case XGMAC_FRP_DEFAULT_MISS_DROP:
		residual_cfg.action.reject_frame = true;
		break;
	default:
		return -EINVAL;
	}

	ret = xgmac_frp_cfg_validate(dev, &residual_cfg);
	if (ret)
		return ret;
	xgmac_frp_cfg_pack(&residual_cfg, &residual_entry);

	return xgmac_frp_entry_write(dev, residual_ok_index, &residual_entry);
}

static uint16_t
xgmac_frp_count_lanes(const struct xgmac_frp_flow_rule *rules, uint16_t nb_rules)
{
	uint16_t total = 0;
	uint16_t i;

	for (i = 0; i < nb_rules; i++) {
		uint16_t first_word, last_word;

		/* Skip zero-size rules to avoid underflowing the lane span below. */
		if (rules[i].match_size == 0)
			continue;

		first_word = rules[i].byte_offset / XGMAC_FRP_WORD_BYTES;
		last_word =
			(rules[i].byte_offset + rules[i].match_size - 1) / XGMAC_FRP_WORD_BYTES;

		total += last_word - first_word + 1;
	}
	return total;
}

int
xgmac_frp_program_rules(struct xgmac_dev *dev, const struct xgmac_frp_flow_rule *rules,
			uint16_t nb_rules)
{
	uint16_t entry_count, residual_ok_index;
	enum xgmac_frp_default_miss policy;
	int ret;

	if (!dev->hw_feat.frp)
		return -ENOTSUP;

	entry_count = dev->hw_feat.frp_entry_count;
	if (entry_count < 2)
		return -ENOSPC;

	if (nb_rules > 0 && !rules)
		return -EINVAL;

	/* Park the residual right after the last rule lane. */
	residual_ok_index = (nb_rules > 0) ? xgmac_frp_count_lanes(rules, nb_rules) : 0;
	if ((uint32_t)residual_ok_index >= entry_count)
		return -ENOSPC;

	policy = dev->flow_isolated ? XGMAC_FRP_DEFAULT_MISS_DROP : XGMAC_FRP_DEFAULT_MISS_ACCEPT;

	/* Disable FRP before programming to avoid transient rule-less state. */
	if (xgmac_rd(dev, XGMAC_MTL_OPMODE) & XGMAC_FRPE) {
		ret = xgmac_frp_enable(dev, false);
		if (ret)
			return ret;
	}

	if (nb_rules > 0) {
		ret = xgmac_frp_program_rules_at(dev, rules, nb_rules, 0,
						 (uint8_t)residual_ok_index);
		if (ret)
			return ret;
	}

	ret = xgmac_frp_program_default_miss_entry(dev, (uint8_t)residual_ok_index, policy,
						   (uint16_t)RTE_BIT32(0));
	if (ret)
		return ret;

	xgmac_frp_grow_nve(dev, residual_ok_index);

	return xgmac_frp_enable(dev, true);
}

int
xgmac_frp_stats_read(struct xgmac_dev *dev)
{
	uint8_t rx_ch_cnt;
	uint8_t ch;
	uint32_t val;
	int ret;

	if (!dev)
		return -EINVAL;
	if (!dev->hw_feat.frp)
		return -ENOTSUP;

	ret = xgmac_iacc_access(dev, XGMAC_ACCSEL_ACC_IFR, XGMAC_FRP_IFR_DROP_CNT, &val, false);
	if (ret)
		return ret;
	dev->mmc_stats.frp_drop_cnt += val & XGMAC_FRP_IFR_RXPDC_MASK;

	ret = xgmac_iacc_access(dev, XGMAC_ACCSEL_ACC_IFR, XGMAC_FRP_IFR_ERROR_CNT, &val, false);
	if (ret)
		return ret;
	dev->mmc_stats.frp_error_cnt += val & XGMAC_FRP_IFR_RXPDC_MASK;

	ret = xgmac_iacc_access(dev, XGMAC_ACCSEL_ACC_IFR, XGMAC_FRP_IFR_BYPASS_CNT, &val, false);
	if (ret)
		return ret;
	dev->mmc_stats.frp_bypass_cnt += val & XGMAC_FRP_IFR_RXPDC_MASK;

	rx_ch_cnt = dev->hw_feat.rx_ch_cnt;
	if (rx_ch_cnt > XGMAC_MAX_QUEUES)
		rx_ch_cnt = XGMAC_MAX_QUEUES;
	for (ch = 0; ch < rx_ch_cnt; ch++) {
		ret = xgmac_iacc_access(dev, XGMAC_ACCSEL_ACC_IFR, XGMAC_FRP_IFR_ACCEPT_CNT(ch),
					&val, false);
		if (ret)
			return ret;
		dev->mmc_stats.frp_accept_cnt[ch] += val & XGMAC_FRP_IFR_RXPDC_MASK;
	}

	return 0;
}

int
xgmac_frp_entry_read(struct xgmac_dev *dev, uint16_t idx, struct xgmac_frp_hw_entry *entry)
{
	uint16_t entry_count;
	uint16_t base;
	uint32_t i;
	int ret;

	entry_count = dev->hw_feat.frp_entry_count;
	if (idx >= entry_count)
		return -EINVAL;

	base = idx * XGMAC_FRP_ENTRY_WORDS;
	for (i = 0; i < XGMAC_FRP_ENTRY_WORDS; i++) {
		uint32_t data = 0;

		ret = xgmac_iacc_access(dev, XGMAC_ACCSEL_ACC_IT, base + i, &data, false);
		if (ret)
			return ret;
		entry->data[i] = data;
	}

	return 0;
}

static int
xgmac_frp_entry_clear(struct xgmac_dev *dev, uint16_t idx)
{
	struct xgmac_frp_hw_entry entry = { { 0 } };

	return xgmac_frp_entry_write(dev, idx, &entry);
}

int
xgmac_frp_table_flush(struct xgmac_dev *dev)
{
	uint16_t entry_count;
	uint16_t idx;
	int ret;

	entry_count = dev->hw_feat.frp_entry_count;

	for (idx = 0; idx < entry_count; idx++) {
		ret = xgmac_frp_entry_clear(dev, idx);
		if (ret)
			return ret;
	}

	return 0;
}

int
xgmac_frp_init(struct xgmac_dev *dev)
{
	uint32_t val;
	int ret;

	if (!dev->hw_feat.frp)
		return 0;

	ret = xgmac_frp_enable(dev, false);
	if (ret)
		return ret;

	ret = xgmac_frp_table_flush(dev);
	if (ret)
		return ret;

	val = xgmac_rd(dev, XGMAC_MTL_RXP_CONTROL_STATUS);
	val &= ~XGMAC_NVE;
	val |= XGMAC_FIELD_PREP(XGMAC_NVE, 0);
	xgmac_wr(dev, XGMAC_MTL_RXP_CONTROL_STATUS, val);

	return 0;
}
