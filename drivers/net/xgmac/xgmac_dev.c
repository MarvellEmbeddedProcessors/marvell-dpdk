/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <errno.h>
#include <string.h>

#include <rte_ether.h>

#include "xgmac_dev.h"
#include "xgmac_ethdev.h"
#include "xgmac_regs.h"

void
xgmac_mac_init(struct xgmac_dev *dev, uint16_t nb_rx_queues)
{
	uint32_t val;
	uint16_t i;

	/* Default packet filter: hash-or-perfect filtering. */
	xgmac_wr(dev, XGMAC_PACKET_FILTER, XGMAC_FILTER_HPF);

	/* TX_CONFIG: set JD, speed = 10G XGMII. */
	val = xgmac_rd(dev, XGMAC_TX_CONFIG);
	val &= ~XGMAC_CONFIG_SS_MASK;
	val |= XGMAC_CONFIG_JD | XGMAC_CONFIG_SS_10000;
	xgmac_wr(dev, XGMAC_TX_CONFIG, val);

	/* RX_CONFIG: set ACS (auto pad/CRC strip). */
	val = xgmac_rd(dev, XGMAC_RX_CONFIG);
	val |= XGMAC_CONFIG_ACS;
	xgmac_wr(dev, XGMAC_RX_CONFIG, val);

	/* Enable Rx queues in RXQ_CTRL0 after all MAC config is done. */
	val = 0;
	for (i = 0; i < nb_rx_queues && i < 8; i++)
		val |= (2u << (i * 2));
	xgmac_wr(dev, XGMAC_RXQ_CTRL0, val);

	XGMAC_LOG(INFO, "MAC init: rx_q=%u, filter=0x%x", nb_rx_queues,
		  xgmac_rd(dev, XGMAC_PACKET_FILTER));
}

static void
xgmac_rx_flow_control_config(struct xgmac_dev *dev, uint16_t queue, uint32_t fifo_per_q)
{
	uint32_t val, flow;
	uint32_t rfd, rfa;

	if (fifo_per_q < 4096)
		return;

	/* Threshold for Deactivating / Activating flow control. */
	if (fifo_per_q == 4096) {
		rfd = 0x03; /* Full - 2.5K */
		rfa = 0x01; /* Full - 1.5K */
	} else {
		rfd = 0x07; /* Full - 4.5K */
		rfa = 0x04; /* Full - 3K */
	}

	/* Write RFA/RFD thresholds before enabling HW flow control. */
	flow = xgmac_rd(dev, XGMAC_MTL_RXQ_FLOW_CONTROL(queue));
	flow &= ~(XGMAC_RFD | XGMAC_RFA);
	flow |= XGMAC_FIELD_PREP(XGMAC_RFD, rfd);
	flow |= XGMAC_FIELD_PREP(XGMAC_RFA, rfa);
	xgmac_wr(dev, XGMAC_MTL_RXQ_FLOW_CONTROL(queue), flow);

	/* Enable HW flow control in MTL_RXQ_OPMODE. */
	val = xgmac_rd(dev, XGMAC_MTL_RXQ_OPMODE(queue));
	val |= XGMAC_EHFC;
	xgmac_wr(dev, XGMAC_MTL_RXQ_OPMODE(queue), val);

	XGMAC_LOG(DEBUG, "Rx q%u flow ctrl: EHFC rfa=0x%x rfd=0x%x", queue, rfa, rfd);
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
		xgmac_wr(dev, XGMAC_MTL_RXQ_OPMODE(i), val);

		/* Configure Rx flow control if per-queue FIFO >= 4 KiB. */
		xgmac_rx_flow_control_config(dev, i, fifo_per_q);
	}

	XGMAC_LOG(DEBUG, "MTL Rx: q=%u fifo_total=%uB fifo/q=%uB RQS=%u", nb_rx_queues, fifo_size,
		  fifo_per_q, qs);

	XGMAC_LOG(INFO, "MTL init: tx_q=%u rx_q=%u", nb_tx_queues, nb_rx_queues);
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

void
xgmac_hw_features_get(struct xgmac_dev *dev)
{
	struct xgmac_hw_features *hw_feat = &dev->hw_feat;
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

	XGMAC_LOG(INFO,
		  "HW features: tx_q=%u rx_q=%u tx_ch=%u rx_ch=%u "
		  "tx_fifo=%uKB rx_fifo=%uKB addrs=%u hash_tbl=%u "
		  "asp=%u dvlan=%u nrvf=%u dma_addr=%u-bit tso=%u tunnel=%u rss=%u",
		  hw_feat->tx_q_cnt, hw_feat->rx_q_cnt, hw_feat->tx_ch_cnt, hw_feat->rx_ch_cnt,
		  XGMAC_FIFO_SIZE(hw_feat->tx_fifo_size) / 1024,
		  XGMAC_FIFO_SIZE(hw_feat->rx_fifo_size) / 1024,
		  hw_feat->addn_mac + 1, hw_feat->hash_table_size, hw_feat->asp, hw_feat->dvlan,
		  hw_feat->nrvf, hw_feat->dma_addr_width, hw_feat->tso, hw_feat->tunnel,
		  hw_feat->rss);
}
