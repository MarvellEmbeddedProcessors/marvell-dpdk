/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 *
 * Synopsys DesignWare XGMAC 10G Ethernet PMD.
 */

#include <bus_platform_driver.h>
#include <errno.h>
#include <ethdev_driver.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_memzone.h>
#include <rte_time.h>

#include "xgmac_dev.h"
#include "xgmac_ethdev.h"
#include "xgmac_regs.h"
#include "xgmac_rxtx.h"

#define XGMAC_PMD_NAME "net_xgmac"
#define PLATFORM_DEVICES_PATH "/sys/bus/platform/devices"
#define XGMAC_DMA_RING_BOUNDARY ((unsigned int)RTE_PGSIZE_4G)

RTE_LOG_REGISTER_DEFAULT(xgmac_logtype, INFO);

static const struct rte_memzone *
xgmac_dma_zone_reserve_bounded(const struct rte_eth_dev *eth_dev, const char *ring_name,
			       uint16_t queue_id, size_t size, unsigned int align,
			       int socket_id)
{
	char z_name[RTE_MEMZONE_NAMESIZE];

	snprintf(z_name, sizeof(z_name), "eth_p%d_q%d_%s",
		 eth_dev->data->port_id, queue_id, ring_name);

	return rte_memzone_reserve_bounded(z_name, size, socket_id,
					    RTE_MEMZONE_IOVA_CONTIG, align,
					    XGMAC_DMA_RING_BOUNDARY);
}

static const struct rte_eth_desc_lim xgmac_rx_desc_lim = {
	.nb_max = XGMAC_MAX_RING_DESC,
	.nb_min = XGMAC_MIN_RING_DESC,
	.nb_align = 8,
};

static const struct rte_eth_desc_lim xgmac_tx_desc_lim = {
	.nb_max = XGMAC_MAX_RING_DESC,
	.nb_min = XGMAC_MIN_RING_DESC,
	.nb_align = 8,
};

static int
xgmac_dev_configure(struct rte_eth_dev *eth_dev)
{
	struct rte_eth_conf *conf = &eth_dev->data->dev_conf;
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	if (eth_dev->data->nb_rx_queues > dev->hw_feat.rx_q_cnt) {
		XGMAC_LOG(ERR, "Rx queues %u exceeds HW max %u", eth_dev->data->nb_rx_queues,
			  dev->hw_feat.rx_q_cnt);
		return -EINVAL;
	}
	if (eth_dev->data->nb_tx_queues > dev->hw_feat.tx_q_cnt) {
		XGMAC_LOG(ERR, "Tx queues %u exceeds HW max %u", eth_dev->data->nb_tx_queues,
			  dev->hw_feat.tx_q_cnt);
		return -EINVAL;
	}

	switch (conf->rxmode.mq_mode) {
	case RTE_ETH_MQ_RX_NONE:
		dev->rss_enable = 0;
		dev->dcb_enable = 0;
		break;
	case RTE_ETH_MQ_RX_RSS:
		if (!dev->hw_feat.rss) {
			XGMAC_LOG(ERR, "RSS not supported by HW");
			return -EINVAL;
		}
		dev->rss_enable = 1;
		dev->dcb_enable = 0;
		break;
	case RTE_ETH_MQ_RX_DCB:
		const struct rte_eth_dcb_rx_conf *dcb = &conf->rx_adv_conf.dcb_rx_conf;
		unsigned int p;

		if (!dev->hw_feat.dcb) {
			XGMAC_LOG(ERR, "DCB not supported by HW (DCBEN=0)");
			return -ENOTSUP;
		}
		if (dcb->nb_tcs != RTE_ETH_4_TCS && dcb->nb_tcs != RTE_ETH_8_TCS) {
			XGMAC_LOG(ERR, "DCB nb_tcs %u must be 4 or 8", dcb->nb_tcs);
			return -EINVAL;
		}
		if (dcb->nb_tcs > dev->hw_feat.tc_cnt) {
			XGMAC_LOG(ERR, "DCB nb_tcs %u exceeds HW TC count %u", dcb->nb_tcs,
				  dev->hw_feat.tc_cnt);
			return -EINVAL;
		}
		if (dcb->nb_tcs > dev->hw_feat.tx_q_cnt) {
			XGMAC_LOG(ERR, "DCB nb_tcs %u exceeds HW Tx queue count %u",
				  dcb->nb_tcs, dev->hw_feat.tx_q_cnt);
			return -EINVAL;
		}
		if (eth_dev->data->nb_rx_queues < dcb->nb_tcs) {
			XGMAC_LOG(ERR, "DCB requires nb_rx_queues (%u) >= nb_tcs (%u)",
				  eth_dev->data->nb_rx_queues, dcb->nb_tcs);
			return -EINVAL;
		}
		for (p = 0; p < RTE_ETH_DCB_NUM_USER_PRIORITIES; p++) {
			if (dcb->dcb_tc[p] >= dcb->nb_tcs) {
				XGMAC_LOG(ERR, "DCB dcb_tc[%u] = %u must be < nb_tcs (%u)", p,
					  dcb->dcb_tc[p], dcb->nb_tcs);
				return -EINVAL;
			}
		}

		dev->rss_enable = 0;
		dev->dcb_enable = 1;
		dev->dcb_nb_tcs = dcb->nb_tcs;
		memcpy(dev->dcb_tc, dcb->dcb_tc, sizeof(dev->dcb_tc));
		break;
	default:
		XGMAC_LOG(ERR, "Rx MQ mode %u not supported", conf->rxmode.mq_mode);
		return -ENOTSUP;
	}
	if (conf->txmode.mq_mode == RTE_ETH_MQ_TX_DCB) {
		XGMAC_LOG(ERR, "Tx DCB mq_mode not supported");
		return -ENOTSUP;
	}

	if ((conf->txmode.offloads & (RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
				      RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
				      RTE_ETH_TX_OFFLOAD_TCP_CKSUM)) &&
	    !dev->hw_feat.tx_coe) {
		XGMAC_LOG(ERR, "Tx checksum offload requested but HW does not support it");
		return -ENOTSUP;
	}

	if ((conf->txmode.offloads & RTE_ETH_TX_OFFLOAD_QINQ_INSERT) &&
	    !dev->hw_feat.dvlan) {
		XGMAC_LOG(ERR, "QinQ insert offload requested but HW does not support double VLAN");
		return -ENOTSUP;
	}
	if ((conf->txmode.offloads & RTE_ETH_TX_OFFLOAD_TCP_TSO) &&
	    !dev->hw_feat.tso) {
		XGMAC_LOG(ERR, "TCP TSO requested but HW does not support TSO");
		return -ENOTSUP;
	}
	if ((conf->txmode.offloads & RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO) &&
	    (!dev->hw_feat.tso || !dev->hw_feat.tunnel)) {
		XGMAC_LOG(ERR, "VxLAN tunnel TSO requested but HW does not support it");
		return -ENOTSUP;
	}
	if ((conf->txmode.offloads & RTE_ETH_TX_OFFLOAD_GRE_TNL_TSO) &&
	    (!dev->hw_feat.tso || !dev->hw_feat.tunnel)) {
		XGMAC_LOG(ERR, "NVGRE tunnel TSO requested but HW does not support it");
		return -ENOTSUP;
	}
	if ((conf->txmode.offloads & RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO) &&
	    (conf->txmode.offloads & RTE_ETH_TX_OFFLOAD_GRE_TNL_TSO)) {
		XGMAC_LOG(ERR, "HW supports only one tunnel mode at a time (VxLAN or NVGRE)");
		return -ENOTSUP;
	}

	if (conf->rxmode.offloads & RTE_ETH_RX_OFFLOAD_TIMESTAMP) {
		if (!dev->hw_feat.ptp) {
			XGMAC_LOG(ERR, "Timestamp offload not supported by HW");
			return -ENOTSUP;
		}
		dev->timestamp_enable = 1;
	} else {
		dev->timestamp_enable = 0;
	}

	return xgmac_tx_offload_update(eth_dev);
}

static int
xgmac_rx_queue_start(struct rte_eth_dev *eth_dev, uint16_t rx_queue_id)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_rx_queue *rxq;
	int ret;

	if (rx_queue_id >= eth_dev->data->nb_rx_queues)
		return -EINVAL;

	rxq = eth_dev->data->rx_queues[rx_queue_id];
	if (rxq == NULL)
		return -EINVAL;

	if (eth_dev->data->rx_queue_state[rx_queue_id] == RTE_ETH_QUEUE_STATE_STARTED)
		return 0;

	ret = xgmac_rxq_start(dev, rxq);
	if (ret)
		return ret;

	eth_dev->data->rx_queue_state[rx_queue_id] = RTE_ETH_QUEUE_STATE_STARTED;

	if (eth_dev->data->dev_started)
		xgmac_rx_offload_update(eth_dev);

	return 0;
}

static int
xgmac_rx_queue_stop(struct rte_eth_dev *eth_dev, uint16_t rx_queue_id)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_rx_queue *rxq;

	if (rx_queue_id >= eth_dev->data->nb_rx_queues)
		return -EINVAL;
	rxq = eth_dev->data->rx_queues[rx_queue_id];
	if (rxq == NULL)
		return -EINVAL;
	if (eth_dev->data->rx_queue_state[rx_queue_id] == RTE_ETH_QUEUE_STATE_STOPPED)
		return 0;

	xgmac_rxq_stop(dev, rxq);
	eth_dev->data->rx_queue_state[rx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;

	return 0;
}

static int
xgmac_tx_queue_start(struct rte_eth_dev *eth_dev, uint16_t tx_queue_id)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_tx_queue *txq;

	if (tx_queue_id >= eth_dev->data->nb_tx_queues)
		return -EINVAL;
	txq = eth_dev->data->tx_queues[tx_queue_id];
	if (txq == NULL)
		return -EINVAL;
	if (eth_dev->data->tx_queue_state[tx_queue_id] == RTE_ETH_QUEUE_STATE_STARTED)
		return 0;

	xgmac_txq_start(dev, txq);
	eth_dev->data->tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STARTED;

	return 0;
}

static int
xgmac_tx_queue_stop(struct rte_eth_dev *eth_dev, uint16_t tx_queue_id)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_tx_queue *txq;

	if (tx_queue_id >= eth_dev->data->nb_tx_queues)
		return -EINVAL;
	txq = eth_dev->data->tx_queues[tx_queue_id];
	if (txq == NULL)
		return -EINVAL;
	if (eth_dev->data->tx_queue_state[tx_queue_id] == RTE_ETH_QUEUE_STATE_STOPPED)
		return 0;

	xgmac_txq_stop(dev, txq);
	eth_dev->data->tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;

	return 0;
}

static int
xgmac_dev_start(struct rte_eth_dev *eth_dev)
{
	uint16_t nb_rx_queues = eth_dev->data->nb_rx_queues;
	uint16_t nb_tx_queues = eth_dev->data->nb_tx_queues;
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint32_t val;
	uint16_t q;
	int ret;

	for (q = 0; q < nb_rx_queues; q++) {
		if (eth_dev->data->rx_queues[q] == NULL) {
			XGMAC_LOG(ERR, "Rx queue %u was not set up", q);
			return -EINVAL;
		}
	}
	for (q = 0; q < nb_tx_queues; q++) {
		if (eth_dev->data->tx_queues[q] == NULL) {
			XGMAC_LOG(ERR, "Tx queue %u was not set up", q);
			return -EINVAL;
		}
	}
	ret = xgmac_dma_init(dev);
	if (ret)
		goto dma_stop;

	xgmac_mac_init(dev, nb_rx_queues);
	xgmac_mac_mtu_set(dev, eth_dev->data->mtu);
	xgmac_mtl_init(dev, nb_tx_queues, nb_rx_queues);
	xgmac_dcb_configure(dev);
	xgmac_flow_ctrl_apply(dev, nb_tx_queues, nb_rx_queues);
	xgmac_pfc_queue_apply(dev, nb_tx_queues, nb_rx_queues);
	xgmac_vlan_insert_cfg(dev);
	xgmac_vlan_strip_cfg(dev);

	ret = xgmac_rss_configure(dev);
	if (ret)
		goto dma_stop;

	/*
	 * Rx timestamping is port-wide; honour it when requested either at port
	 * level (already reflected in dev->timestamp_enable) or on any Rx queue.
	 */
	if (dev->hw_feat.ptp && !dev->timestamp_enable) {
		for (q = 0; q < nb_rx_queues; q++) {
			struct xgmac_rx_queue *rxq = eth_dev->data->rx_queues[q];

			if (rxq->offloads & RTE_ETH_RX_OFFLOAD_TIMESTAMP) {
				dev->timestamp_enable = 1;
				break;
			}
		}
	}

	ret = xgmac_timestamp_configure(dev);
	if (ret)
		goto dma_stop;

	ret = xgmac_frp_init(dev);
	if (ret)
		goto timestamp_disable;

	xgmac_mmc_init(dev);

	dev->rx_buf_size = 0;
	for (q = 0; q < nb_rx_queues; q++) {
		struct xgmac_rx_queue *rxq = eth_dev->data->rx_queues[q];

		/* Deferred queues will be started later using rte_eth_dev_rx_queue_start(). */
		if (rxq->deferred_start) {
			eth_dev->data->rx_queue_state[q] = RTE_ETH_QUEUE_STATE_STOPPED;
			continue;
		}
		ret = xgmac_rx_queue_start(eth_dev, q);
		if (ret)
			goto stop_queues;
	}

	for (q = 0; q < nb_tx_queues; q++) {
		struct xgmac_tx_queue *txq = eth_dev->data->tx_queues[q];

		/* Deferred queues will be started later using rte_eth_dev_tx_queue_start(). */
		if (txq->deferred_start) {
			eth_dev->data->tx_queue_state[q] = RTE_ETH_QUEUE_STATE_STOPPED;
			continue;
		}
		ret = xgmac_tx_queue_start(eth_dev, q);
		if (ret)
			goto stop_queues;
	}

	val = xgmac_rd(dev, XGMAC_TX_CONFIG);
	xgmac_wr(dev, XGMAC_TX_CONFIG, val | XGMAC_CONFIG_TE);

	val = xgmac_rd(dev, XGMAC_RX_CONFIG);
	xgmac_wr(dev, XGMAC_RX_CONFIG, val | XGMAC_CONFIG_RE);

	eth_dev->data->dev_started = 1;
	xgmac_rx_offload_update(eth_dev);
	xgmac_tx_offload_update(eth_dev);

	return 0;

stop_queues:
	for (q = 0; q < nb_rx_queues; q++) {
		if (eth_dev->data->rx_queue_state[q] == RTE_ETH_QUEUE_STATE_STARTED)
			xgmac_rx_queue_stop(eth_dev, q);
	}
	for (q = 0; q < nb_tx_queues; q++) {
		if (eth_dev->data->tx_queue_state[q] == RTE_ETH_QUEUE_STATE_STARTED)
			xgmac_tx_queue_stop(eth_dev, q);
	}
timestamp_disable:
	if (dev->timestamp_enable || dev->timesync_enable)
		xgmac_timestamp_disable(dev);
dma_stop:
	xgmac_dma_stop(dev);

	return ret;
}

static int
xgmac_dev_stop(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint16_t q;
	int ret;

	if (!eth_dev->data->dev_started)
		return 0;

	if (dev->timestamp_enable || dev->timesync_enable)
		xgmac_timestamp_disable(dev);

	for (q = 0; q < eth_dev->data->nb_rx_queues; q++) {
		if (eth_dev->data->rx_queues[q] == NULL)
			continue;
		xgmac_rx_queue_stop(eth_dev, q);
	}
	for (q = 0; q < eth_dev->data->nb_tx_queues; q++) {
		if (eth_dev->data->tx_queues[q] == NULL)
			continue;
		xgmac_tx_queue_stop(eth_dev, q);
	}
	if (dev->hw_feat.frp) {
		ret = xgmac_frp_enable(dev, false);
		if (ret)
			XGMAC_LOG(WARNING, "failed to disable FRP on stop: %d", ret);
	}
	xgmac_dma_stop(dev);
	eth_dev->data->dev_started = 0;

	return 0;
}

static inline bool
xgmac_vlan_type_is_svlan(uint16_t tpid)
{
	return tpid == RTE_ETHER_TYPE_QINQ ||
	       tpid == RTE_ETHER_TYPE_QINQ1 ||
	       tpid == RTE_ETHER_TYPE_QINQ2 ||
	       tpid == RTE_ETHER_TYPE_QINQ3;
}

static inline bool
xgmac_vlan_type_combo_valid(const struct xgmac_dev *dev)
{
	/* XGMAC does not support outer C-VLAN + inner S-VLAN sequence. */
	return !(!dev->vlan_outer_svlan && dev->vlan_inner_svlan);
}

static void
xgmac_rx_queue_free(struct xgmac_rx_queue *rxq)
{
	if (!rxq)
		return;

	xgmac_rxq_release_mbufs(rxq);

	if (rxq->sw_ring)
		rte_free(rxq->sw_ring);
	if (rxq->mz)
		rte_memzone_free(rxq->mz);
	rte_free(rxq);
}

static void
xgmac_tx_queue_free(struct xgmac_tx_queue *txq)
{
	uint16_t i;

	if (!txq)
		return;

	if (txq->sw_ring) {
		for (i = 0; i < txq->nb_desc; i++) {
			if (txq->sw_ring[i])
				rte_pktmbuf_free(txq->sw_ring[i]);
		}
		rte_free(txq->sw_ring);
	}
	if (txq->mz)
		rte_memzone_free(txq->mz);
	rte_free(txq);
}

static int
xgmac_dev_close(struct rte_eth_dev *eth_dev)
{
	uint16_t i;

	if (eth_dev->data->dev_started)
		xgmac_dev_stop(eth_dev);

	for (i = 0; i < eth_dev->data->nb_rx_queues; i++) {
		xgmac_rx_queue_free(eth_dev->data->rx_queues[i]);
		eth_dev->data->rx_queues[i] = NULL;
	}
	for (i = 0; i < eth_dev->data->nb_tx_queues; i++) {
		xgmac_tx_queue_free(eth_dev->data->tx_queues[i]);
		eth_dev->data->tx_queues[i] = NULL;
	}
	return 0;
}

static int
xgmac_rx_queue_setup(struct rte_eth_dev *dev, uint16_t rx_queue_id,
		     uint16_t nb_rx_desc, unsigned int socket_id,
		     const struct rte_eth_rxconf *rx_conf,
		     struct rte_mempool *mb_pool)
{
	struct xgmac_rx_queue *rxq;
	const struct rte_memzone *mz;
	uint16_t rx_free_thresh;
	size_t size;

	if (nb_rx_desc < XGMAC_MIN_RING_DESC || nb_rx_desc > XGMAC_MAX_RING_DESC ||
	    !rte_is_power_of_2(nb_rx_desc))
		return -EINVAL;

	rx_free_thresh =
		rx_conf->rx_free_thresh ? rx_conf->rx_free_thresh : XGMAC_DEFAULT_RX_FREE_THRESH;

	if (rx_free_thresh >= nb_rx_desc || nb_rx_desc % rx_free_thresh != 0) {
		XGMAC_LOG(ERR, "rx_free_thresh %u invalid for nb_rx_desc %u", rx_free_thresh,
			  nb_rx_desc);
		return -EINVAL;
	}

	if (dev->data->dev_started)
		return -EBUSY;

	if (dev->data->rx_queues[rx_queue_id]) {
		xgmac_rx_queue_free(dev->data->rx_queues[rx_queue_id]);
		dev->data->rx_queues[rx_queue_id] = NULL;
		dev->data->rx_queue_state[rx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;
	}

	rxq = rte_zmalloc_socket("xgmac_rxq", sizeof(*rxq), RTE_CACHE_LINE_SIZE, socket_id);
	if (!rxq)
		return -ENOMEM;

	rxq->dev = dev->data->dev_private;
	rxq->nb_desc = nb_rx_desc;
	rxq->rx_free_thresh = rx_free_thresh;
	rxq->queue_id = rx_queue_id;
	rxq->port_id = dev->data->port_id;
	rxq->mb_pool = mb_pool;
	rxq->cur = 0;
	rxq->dirty = 0;

	if (dev->data->dev_conf.rxmode.offloads & RTE_ETH_RX_OFFLOAD_KEEP_CRC)
		rxq->crc_adj = RTE_ETHER_CRC_LEN;
	else
		rxq->crc_adj = 0;

	rxq->offloads = rx_conf->offloads | dev->data->dev_conf.rxmode.offloads;
	rxq->deferred_start = rx_conf->rx_deferred_start;

	size = nb_rx_desc * sizeof(union xgmac_rx_desc);
	mz = xgmac_dma_zone_reserve_bounded(dev, "rx_ring", rx_queue_id, size,
					    XGMAC_DESC_ALIGN, socket_id);
	if (!mz) {
		XGMAC_LOG(ERR, "rx_ring dma zone reserve bounded failed");
		rte_free(rxq);
		return -ENOMEM;
	}
	rxq->ring_phys_addr = mz->iova;
	rxq->mz = mz;
	memset(mz->addr, 0, size);
	rxq->desc = mz->addr;

	size = nb_rx_desc * sizeof(struct rte_mbuf *);
	rxq->sw_ring = rte_zmalloc_socket("xgmac_rx_sw", size, RTE_CACHE_LINE_SIZE, socket_id);
	if (!rxq->sw_ring) {
		XGMAC_LOG(ERR, "rx sw_ring alloc failed");
		rte_memzone_free(mz);
		rte_free(rxq);
		return -ENOMEM;
	}

	dev->data->rx_queues[rx_queue_id] = rxq;
	return 0;
}

static void
xgmac_rx_queue_release(struct rte_eth_dev *dev, uint16_t rx_queue_id)
{
	struct xgmac_rx_queue *rxq = dev->data->rx_queues[rx_queue_id];

	if (dev->data->dev_started) {
		XGMAC_LOG(ERR, "cannot release rx queue %u while device is started",
			  rx_queue_id);
		return;
	}
	xgmac_rx_queue_free(rxq);
	dev->data->rx_queues[rx_queue_id] = NULL;
	dev->data->rx_queue_state[rx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;
}

static int
xgmac_tx_queue_setup(struct rte_eth_dev *eth_dev, uint16_t tx_queue_id,
		     uint16_t nb_tx_desc, unsigned int socket_id,
		     const struct rte_eth_txconf *tx_conf)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	const struct rte_memzone *mz;
	struct xgmac_tx_queue *txq;
	uint32_t fifo_per_q;
	size_t size;

	if (nb_tx_desc < XGMAC_MIN_RING_DESC || nb_tx_desc > XGMAC_MAX_RING_DESC ||
	    !rte_is_power_of_2(nb_tx_desc))
		return -EINVAL;

	if (eth_dev->data->dev_started)
		return -EBUSY;

	if (eth_dev->data->tx_queues[tx_queue_id]) {
		xgmac_tx_queue_free(eth_dev->data->tx_queues[tx_queue_id]);
		eth_dev->data->tx_queues[tx_queue_id] = NULL;
		eth_dev->data->tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;
	}
	txq = rte_zmalloc_socket("xgmac_txq", sizeof(*txq), RTE_CACHE_LINE_SIZE, socket_id);
	if (!txq)
		return -ENOMEM;

	txq->nb_desc = nb_tx_desc;
	txq->queue_id = tx_queue_id;
	txq->port_id = eth_dev->data->port_id;
	txq->free_thresh = tx_conf->tx_free_thresh ?
		tx_conf->tx_free_thresh : XGMAC_DEFAULT_TX_FREE_THRESH;
	if (txq->free_thresh > txq->nb_desc)
		txq->free_thresh = (txq->nb_desc >> 1);
	if (txq->free_thresh == 0)
		txq->free_thresh = 1;
	txq->cur = 0;
	txq->dirty = 0;
	txq->offloads = tx_conf->offloads | eth_dev->data->dev_conf.txmode.offloads;
	txq->deferred_start = tx_conf->tx_deferred_start;
	txq->vlan_ctx_valid = 0;
	txq->vlan_ctx_qinq = 0;
	txq->vlan_ctx_outer_tci = 0;
	txq->vlan_ctx_inner_tci = 0;
	txq->tso_mss = 0;
	txq->tso_mss_valid = 0;

	fifo_per_q = XGMAC_FIFO_SIZE(dev->hw_feat.tx_fifo_size);
	if (eth_dev->data->nb_tx_queues)
		fifo_per_q /= eth_dev->data->nb_tx_queues;
	txq->tso_max_seg_len = (uint16_t)RTE_MIN((uint32_t)XGMAC_TSO_MAX_SEG_LEN, fifo_per_q / 2);

	size = nb_tx_desc * sizeof(union xgmac_tx_desc);
	mz = xgmac_dma_zone_reserve_bounded(eth_dev, "tx_ring", tx_queue_id, size,
					    XGMAC_DESC_ALIGN, socket_id);
	if (!mz) {
		XGMAC_LOG(ERR, "tx_ring dma zone reserve bounded failed");
		rte_free(txq);
		return -ENOMEM;
	}
	txq->ring_phys_addr = mz->iova;
	txq->mz = mz;
	memset(mz->addr, 0, size);
	txq->desc = mz->addr;

	size = nb_tx_desc * sizeof(struct rte_mbuf *);
	txq->sw_ring = rte_zmalloc_socket("xgmac_tx_sw", size, RTE_CACHE_LINE_SIZE, socket_id);
	if (!txq->sw_ring) {
		XGMAC_LOG(ERR, "tx sw_ring alloc failed");
		rte_memzone_free(mz);
		rte_free(txq);
		return -ENOMEM;
	}

	eth_dev->data->tx_queues[tx_queue_id] = txq;
	return 0;
}

static void
xgmac_tx_queue_release(struct rte_eth_dev *dev, uint16_t tx_queue_id)
{
	struct xgmac_tx_queue *txq = dev->data->tx_queues[tx_queue_id];

	if (dev->data->dev_started) {
		XGMAC_LOG(ERR, "cannot release tx queue %u while device is started",
			  tx_queue_id);
		return;
	}
	xgmac_tx_queue_free(txq);
	dev->data->tx_queues[tx_queue_id] = NULL;
	dev->data->tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;
}

static void
xgmac_rxq_info_get(struct rte_eth_dev *eth_dev, uint16_t rx_queue_id,
		   struct rte_eth_rxq_info *qinfo)
{
	struct xgmac_rx_queue *rxq = eth_dev->data->rx_queues[rx_queue_id];

	if (rxq == NULL || qinfo == NULL)
		return;

	qinfo->mp = rxq->mb_pool;
	qinfo->nb_desc = rxq->nb_desc;
	qinfo->rx_buf_size = rxq->buf_size;
	qinfo->scattered_rx = eth_dev->data->scattered_rx;
	qinfo->queue_state = eth_dev->data->rx_queue_state[rx_queue_id];
	qinfo->conf.rx_free_thresh = rxq->rx_free_thresh;
	qinfo->conf.offloads = rxq->offloads;
	qinfo->conf.rx_deferred_start = rxq->deferred_start;
}

static void
xgmac_txq_info_get(struct rte_eth_dev *eth_dev, uint16_t tx_queue_id,
		   struct rte_eth_txq_info *qinfo)
{
	struct xgmac_tx_queue *txq = eth_dev->data->tx_queues[tx_queue_id];

	if (txq == NULL || qinfo == NULL)
		return;

	qinfo->nb_desc = txq->nb_desc;
	qinfo->queue_state = eth_dev->data->tx_queue_state[tx_queue_id];
	qinfo->conf.tx_free_thresh = txq->free_thresh;
	qinfo->conf.offloads = txq->offloads;
	qinfo->conf.tx_deferred_start = txq->deferred_start;
}

static int
xgmac_dev_infos_get(struct rte_eth_dev *eth_dev, struct rte_eth_dev_info *info)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	info->max_rx_queues = dev->hw_feat.rx_q_cnt;
	info->max_tx_queues = dev->hw_feat.tx_q_cnt;
	info->max_mac_addrs = dev->hw_feat.addn_mac + 1;
	info->speed_capa = RTE_ETH_LINK_SPEED_10G;
	info->max_rx_pktlen = XGMAC_JUMBO_LEN;
	info->min_mtu = RTE_ETHER_MIN_MTU;
	info->max_mtu = XGMAC_JUMBO_LEN - RTE_ETHER_HDR_LEN - RTE_ETHER_CRC_LEN;
	info->rx_desc_lim = xgmac_rx_desc_lim;
	info->tx_desc_lim = xgmac_tx_desc_lim;
	info->default_rxconf.rx_free_thresh = XGMAC_DEFAULT_RX_FREE_THRESH;
	info->default_txconf.tx_free_thresh = XGMAC_DEFAULT_TX_FREE_THRESH;
	info->rx_offload_capa = RTE_ETH_RX_OFFLOAD_SCATTER | RTE_ETH_RX_OFFLOAD_RSS_HASH |
				RTE_ETH_RX_OFFLOAD_VLAN_STRIP | RTE_ETH_RX_OFFLOAD_KEEP_CRC;
	if (dev->hw_feat.dvlan)
		info->rx_offload_capa |= RTE_ETH_RX_OFFLOAD_QINQ_STRIP;
	if (dev->hw_feat.rx_coe)
		info->rx_offload_capa |= RTE_ETH_RX_OFFLOAD_IPV4_CKSUM |
					 RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
					 RTE_ETH_RX_OFFLOAD_TCP_CKSUM;
	if (dev->hw_feat.ptp) {
		info->rx_offload_capa |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
		info->rx_queue_offload_capa |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
	}
	info->tx_offload_capa = RTE_ETH_TX_OFFLOAD_MULTI_SEGS |
				RTE_ETH_TX_OFFLOAD_VLAN_INSERT;

	if (dev->hw_feat.tx_coe)
		info->tx_offload_capa |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
					 RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
					 RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
	if (dev->hw_feat.tso)
		info->tx_offload_capa |= RTE_ETH_TX_OFFLOAD_TCP_TSO;
	if (dev->hw_feat.tunnel)
		info->tx_offload_capa |= RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM |
					 RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM;
	if (dev->hw_feat.tso && dev->hw_feat.tunnel)
		info->tx_offload_capa |= RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO |
					 RTE_ETH_TX_OFFLOAD_GRE_TNL_TSO;
	if (dev->hw_feat.dvlan)
		info->tx_offload_capa |= RTE_ETH_TX_OFFLOAD_QINQ_INSERT;

	if (dev->hw_feat.rss) {
		info->reta_size =
			dev->rss_table_size ? dev->rss_table_size : XGMAC_RSS_MAX_TABLE_SIZE;
		info->hash_key_size = XGMAC_RSS_HASH_KEY_SIZE;
		info->flow_type_rss_offloads = XGMAC_RSS_OFFLOAD;
	}

	return 0;
}

static int
xgmac_link_update(struct rte_eth_dev *eth_dev, int wait_to_complete __rte_unused)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct rte_eth_link link;

	memset(&link, 0, sizeof(link));
	link.link_speed = RTE_ETH_SPEED_NUM_10G;
	link.link_duplex = RTE_ETH_LINK_FULL_DUPLEX;
	link.link_autoneg = RTE_ETH_LINK_FIXED;
	link.link_status = (eth_dev->data->dev_started && !dev->link_down) ? RTE_ETH_LINK_UP :
									     RTE_ETH_LINK_DOWN;

	return rte_eth_linkstatus_set(eth_dev, &link);
}

static int
xgmac_mac_addr_set(struct rte_eth_dev *eth_dev, struct rte_ether_addr *mac_addr)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	xgmac_mac_addr_write(dev, 0, mac_addr);

	XGMAC_LOG(DEBUG, "MAC addr set: " RTE_ETHER_ADDR_PRT_FMT, RTE_ETHER_ADDR_BYTES(mac_addr));
	return 0;
}

static int
xgmac_promiscuous_enable(struct rte_eth_dev *eth_dev)
{
	xgmac_mac_promiscuous_set(eth_dev->data->dev_private, true);

	return 0;
}

static int
xgmac_promiscuous_disable(struct rte_eth_dev *eth_dev)
{
	xgmac_mac_promiscuous_set(eth_dev->data->dev_private, false);

	return 0;
}

static int
xgmac_allmulticast_enable(struct rte_eth_dev *eth_dev)
{
	xgmac_mac_allmulticast_set(eth_dev->data->dev_private, true);

	return 0;
}

static int
xgmac_allmulticast_disable(struct rte_eth_dev *eth_dev)
{
	xgmac_mac_allmulticast_set(eth_dev->data->dev_private, false);

	return 0;
}

static int
xgmac_stats_get(struct rte_eth_dev *eth_dev, struct rte_eth_stats *stats,
		struct eth_queue_stats *qstats)
{
	struct xgmac_rx_queue *rxq;
	struct xgmac_tx_queue *txq;
	uint16_t i;

	memset(stats, 0, sizeof(*stats));

	for (i = 0; i < eth_dev->data->nb_rx_queues; i++) {
		rxq = eth_dev->data->rx_queues[i];
		if (!rxq)
			continue;
		stats->ipackets += rxq->nb_pkts;
		stats->ibytes += rxq->nb_bytes;
		stats->ierrors += rxq->errors;
		stats->rx_nombuf += rxq->rx_mbuf_alloc_failed;
		if (qstats != NULL && i < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
			qstats->q_ipackets[i] = rxq->nb_pkts;
			qstats->q_ibytes[i] = rxq->nb_bytes;
			qstats->q_errors[i] = rxq->errors;
		}
	}
	for (i = 0; i < eth_dev->data->nb_tx_queues; i++) {
		txq = eth_dev->data->tx_queues[i];
		if (!txq)
			continue;
		stats->opackets += txq->nb_pkts;
		stats->obytes += txq->nb_bytes;
		stats->oerrors += txq->errors;
		if (qstats != NULL && i < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
			qstats->q_opackets[i] = txq->nb_pkts;
			qstats->q_obytes[i] = txq->nb_bytes;
		}
	}

	return 0;
}

static int
xgmac_stats_reset(struct rte_eth_dev *eth_dev)
{
	struct xgmac_rx_queue *rxq;
	struct xgmac_tx_queue *txq;
	uint16_t i;

	for (i = 0; i < eth_dev->data->nb_rx_queues; i++) {
		rxq = eth_dev->data->rx_queues[i];
		if (rxq) {
			rxq->nb_pkts = 0;
			rxq->nb_bytes = 0;
			rxq->errors = 0;
			rxq->rx_mbuf_alloc_failed = 0;
		}
	}

	for (i = 0; i < eth_dev->data->nb_tx_queues; i++) {
		txq = eth_dev->data->tx_queues[i];
		if (txq) {
			txq->nb_pkts = 0;
			txq->nb_bytes = 0;
			txq->errors = 0;
			txq->tso_rejected = 0;
		}
	}

	return 0;
}

struct xgmac_xstat_desc {
	const char name[RTE_ETH_XSTATS_NAME_SIZE];
	uint32_t offset;
};

#define XGMAC_XSTAT(_name, _field) { \
	.name = _name, \
	.offset = offsetof(struct xgmac_mmc_stats, _field), \
}

/* Per-DMA-channel FRP accept counters.  Indexed by channel. */
#define XGMAC_FRP_ACCEPT_CH_XSTAT(_ch)                                                             \
	{                                                                                          \
		.name = "frp_accept_ch" #_ch,                                                      \
		.offset = offsetof(struct xgmac_mmc_stats, frp_accept_cnt[_ch]),                   \
	}

static const struct xgmac_xstat_desc xgmac_xstats_strings[] = {
	XGMAC_XSTAT("tx_octet_count_gb", tx_octet_count_gb),
	XGMAC_XSTAT("tx_frame_count_gb", tx_frame_count_gb),
	XGMAC_XSTAT("tx_underflow_error", tx_underflow_error),
	XGMAC_XSTAT("tx_pause_frames", tx_pause_frames),
	XGMAC_XSTAT("rx_frame_count_gb", rx_frame_count_gb),
	XGMAC_XSTAT("rx_octet_count_gb", rx_octet_count_gb),
	XGMAC_XSTAT("rx_crc_error", rx_crc_error),
	XGMAC_XSTAT("rx_runt_error", rx_runt_error),
	XGMAC_XSTAT("rx_jabber_error", rx_jabber_error),
	XGMAC_XSTAT("rx_length_error", rx_length_error),
	XGMAC_XSTAT("rx_fifo_overflow", rx_fifo_overflow),
	XGMAC_XSTAT("rx_pause_frames", rx_pause_frames),
	XGMAC_XSTAT("frp_drop_cnt", frp_drop_cnt),
	XGMAC_XSTAT("frp_error_cnt", frp_error_cnt),
	XGMAC_XSTAT("frp_bypass_cnt", frp_bypass_cnt),
	XGMAC_XSTAT("tx_tso_rejected", tx_tso_rejected),
	XGMAC_FRP_ACCEPT_CH_XSTAT(0),
	XGMAC_FRP_ACCEPT_CH_XSTAT(1),
	XGMAC_FRP_ACCEPT_CH_XSTAT(2),
	XGMAC_FRP_ACCEPT_CH_XSTAT(3),
	XGMAC_FRP_ACCEPT_CH_XSTAT(4),
	XGMAC_FRP_ACCEPT_CH_XSTAT(5),
	XGMAC_FRP_ACCEPT_CH_XSTAT(6),
	XGMAC_FRP_ACCEPT_CH_XSTAT(7),
};

#define XGMAC_NB_XSTATS RTE_DIM(xgmac_xstats_strings)

static int
xgmac_xstats_get_names(struct rte_eth_dev *eth_dev __rte_unused,
		       struct rte_eth_xstat_name *xstats_names, unsigned int size)
{
	unsigned int i;

	if (xstats_names == NULL || size < XGMAC_NB_XSTATS)
		return XGMAC_NB_XSTATS;

	for (i = 0; i < XGMAC_NB_XSTATS; i++)
		snprintf(xstats_names[i].name, RTE_ETH_XSTATS_NAME_SIZE, "%s",
			 xgmac_xstats_strings[i].name);

	return XGMAC_NB_XSTATS;
}

static int
xgmac_xstats_get(struct rte_eth_dev *eth_dev, struct rte_eth_xstat *xstats,
		 unsigned int n)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_tx_queue *txq;
	uint64_t tso_rejected = 0;
	unsigned int i;

	if (xstats == NULL || n < XGMAC_NB_XSTATS)
		return XGMAC_NB_XSTATS;

	xgmac_mmc_stats_read(dev);
	if (dev->hw_feat.frp)
		xgmac_frp_stats_read(dev);

	for (i = 0; i < eth_dev->data->nb_tx_queues; i++) {
		txq = eth_dev->data->tx_queues[i];
		if (txq)
			tso_rejected += txq->tso_rejected;
	}
	dev->mmc_stats.tx_tso_rejected = tso_rejected;

	for (i = 0; i < XGMAC_NB_XSTATS; i++) {
		xstats[i].id = i;
		xstats[i].value = *(uint64_t *)((uint8_t *)&dev->mmc_stats +
						xgmac_xstats_strings[i].offset);
	}
	return XGMAC_NB_XSTATS;
}

static int
xgmac_xstats_reset(struct rte_eth_dev *eth_dev)
{
	return xgmac_stats_reset(eth_dev);
}

static int
xgmac_mtu_set(struct rte_eth_dev *eth_dev, uint16_t mtu)
{
	xgmac_mac_mtu_set(eth_dev->data->dev_private, mtu);

	XGMAC_LOG(INFO, "MTU set to %u", mtu);
	return 0;
}

static int
xgmac_dev_set_link_up(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	dev->link_down = 0;
	xgmac_link_update(eth_dev, 0);

	XGMAC_LOG(INFO, "Link set up");
	return 0;
}

static int
xgmac_dev_set_link_down(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	dev->link_down = 1;
	xgmac_link_update(eth_dev, 0);

	XGMAC_LOG(INFO, "Link set down");
	return 0;
}

static int
xgmac_mac_addr_add(struct rte_eth_dev *eth_dev, struct rte_ether_addr *mac_addr, uint32_t index,
		   uint32_t pool __rte_unused)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	if (index > dev->hw_feat.addn_mac) {
		XGMAC_LOG(ERR, "Invalid MAC address index %u (max %u)", index,
			  dev->hw_feat.addn_mac);
		return -EINVAL;
	}

	xgmac_mac_addr_write(dev, index, mac_addr);

	XGMAC_LOG(DEBUG, "MAC addr added at index %u: " RTE_ETHER_ADDR_PRT_FMT, index,
		  RTE_ETHER_ADDR_BYTES(mac_addr));
	return 0;
}

static int
xgmac_dev_get_dcb_info(struct rte_eth_dev *eth_dev, struct rte_eth_dcb_info *dcb_info)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint8_t i, p;

	dcb_info->nb_tcs = dev->dcb_enable ? dev->dcb_nb_tcs : 1;

	if (dev->dcb_enable) {
		for (p = 0; p < RTE_ETH_DCB_NUM_USER_PRIORITIES; p++)
			dcb_info->prio_tc[p] = dev->dcb_tc[p];
	}

	for (i = 0; i < dcb_info->nb_tcs; i++) {
		dcb_info->tc_queue.tc_rxq[0][i].base = i;
		dcb_info->tc_queue.tc_rxq[0][i].nb_queue = 1;
		dcb_info->tc_queue.tc_txq[0][i].base = i;
		dcb_info->tc_queue.tc_txq[0][i].nb_queue = 1;
	}

	return 0;
}

static void
xgmac_mac_addr_remove(struct rte_eth_dev *eth_dev, uint32_t index)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	if (index > dev->hw_feat.addn_mac) {
		XGMAC_LOG(ERR, "Invalid MAC address index %u", index);
		return;
	}

	xgmac_mac_addr_clear(dev, index);

	XGMAC_LOG(DEBUG, "MAC addr removed at index %u", index);
}

static int
xgmac_set_mc_addr_list(struct rte_eth_dev *eth_dev, struct rte_ether_addr *mc_addr_set,
		       uint32_t nb_mc_addr)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	int ret;

	ret = xgmac_mc_hash_filter_set(dev, mc_addr_set, nb_mc_addr);
	if (ret) {
		XGMAC_LOG(ERR, "Failed to set MC hash filter");
		return ret;
	}

	XGMAC_LOG(DEBUG, "MC addr list: %u addrs", nb_mc_addr);
	return 0;
}

static int
xgmac_dev_rss_reta_update(struct rte_eth_dev *eth_dev, struct rte_eth_rss_reta_entry64 *reta_conf,
			  uint16_t reta_size)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	unsigned int i, idx, shift;

	if (!dev->rss_enable) {
		XGMAC_LOG(ERR, "RSS not enabled");
		return -ENOTSUP;
	}

	if (reta_size == 0 || reta_size > dev->rss_table_size) {
		XGMAC_LOG(ERR, "reta_size %u is not supported (max %u)", reta_size,
			  dev->rss_table_size);
		return -EINVAL;
	}

	for (i = 0; i < reta_size; i++) {
		idx = i / RTE_ETH_RETA_GROUP_SIZE;
		shift = i % RTE_ETH_RETA_GROUP_SIZE;

		if (reta_conf[idx].mask & (1ULL << shift))
			dev->rss_table[i] = reta_conf[idx].reta[shift];
	}

	return xgmac_write_rss_lookup_table(dev);
}

static int
xgmac_dev_rss_reta_query(struct rte_eth_dev *eth_dev, struct rte_eth_rss_reta_entry64 *reta_conf,
			 uint16_t reta_size)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	unsigned int i, idx, shift;

	if (!dev->rss_enable) {
		XGMAC_LOG(ERR, "RSS not enabled");
		return -ENOTSUP;
	}

	if (reta_size == 0 || reta_size > dev->rss_table_size) {
		XGMAC_LOG(ERR, "reta_size %u is not supported (max %u)", reta_size,
			  dev->rss_table_size);
		return -EINVAL;
	}

	for (i = 0; i < reta_size; i++) {
		idx = i / RTE_ETH_RETA_GROUP_SIZE;
		shift = i % RTE_ETH_RETA_GROUP_SIZE;

		if (reta_conf[idx].mask & (1ULL << shift))
			reta_conf[idx].reta[shift] = dev->rss_table[i];
	}

	return 0;
}

static int
xgmac_dev_rss_hash_update(struct rte_eth_dev *eth_dev, struct rte_eth_rss_conf *rss_conf)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	int ret;

	if (!dev->rss_enable) {
		XGMAC_LOG(ERR, "RSS not enabled");
		return -ENOTSUP;
	}

	if (rss_conf == NULL)
		return -EINVAL;

	if (rss_conf->rss_key != NULL && rss_conf->rss_key_len == XGMAC_RSS_HASH_KEY_SIZE) {
		memcpy(dev->rss_key, rss_conf->rss_key, XGMAC_RSS_HASH_KEY_SIZE);
		ret = xgmac_write_rss_hash_key(dev);
		if (ret)
			return ret;
	}

	dev->rss_hf = rss_conf->rss_hf & XGMAC_RSS_OFFLOAD;

	dev->rss_options = 0;
	if (dev->rss_hf & (RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_IPV6))
		dev->rss_options |= XGMAC_IP2TE;
	if (dev->rss_hf & (RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV6_TCP))
		dev->rss_options |= XGMAC_TCP4TE;
	if (dev->rss_hf & (RTE_ETH_RSS_NONFRAG_IPV4_UDP | RTE_ETH_RSS_NONFRAG_IPV6_UDP))
		dev->rss_options |= XGMAC_UDP4TE;

	xgmac_wr(dev, XGMAC_RSS_CTRL, dev->rss_options | XGMAC_RSSE);

	return 0;
}

static int
xgmac_dev_rss_hash_conf_get(struct rte_eth_dev *eth_dev, struct rte_eth_rss_conf *rss_conf)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	if (!dev->rss_enable) {
		XGMAC_LOG(ERR, "RSS not enabled");
		return -ENOTSUP;
	}

	if (rss_conf == NULL)
		return -EINVAL;

	if (rss_conf->rss_key != NULL && rss_conf->rss_key_len >= XGMAC_RSS_HASH_KEY_SIZE)
		memcpy(rss_conf->rss_key, dev->rss_key, XGMAC_RSS_HASH_KEY_SIZE);

	rss_conf->rss_key_len = XGMAC_RSS_HASH_KEY_SIZE;
	rss_conf->rss_hf = dev->rss_hf;
	return 0;
}

static int
xgmac_flow_ctrl_get(struct rte_eth_dev *eth_dev, struct rte_eth_fc_conf *fc_conf)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	bool tx_pause, rx_pause;
	uint32_t txfc;

	if (fc_conf == NULL)
		return -EINVAL;

	txfc = xgmac_rd(dev, XGMAC_Qx_TX_FLOW_CTRL(0));
	tx_pause = !!(txfc & XGMAC_TFE);
	rx_pause = !!(xgmac_rd(dev, XGMAC_RX_FLOW_CTRL) & XGMAC_RFE);

	if (tx_pause && rx_pause)
		fc_conf->mode = RTE_ETH_FC_FULL;
	else if (rx_pause)
		fc_conf->mode = RTE_ETH_FC_RX_PAUSE;
	else if (tx_pause)
		fc_conf->mode = RTE_ETH_FC_TX_PAUSE;
	else
		fc_conf->mode = RTE_ETH_FC_NONE;

	fc_conf->autoneg = dev->pause_autoneg;
	fc_conf->pause_time = XGMAC_FIELD_GET(XGMAC_PT, txfc);
	fc_conf->high_water = dev->fc_high_water;
	fc_conf->low_water = dev->fc_low_water;
	fc_conf->send_xon = 0;

	return 0;
}

static int
xgmac_flow_ctrl_set(struct rte_eth_dev *eth_dev, struct rte_eth_fc_conf *fc_conf)
{
	uint16_t nb_rx_queues = eth_dev->data->nb_rx_queues;
	uint16_t nb_tx_queues = eth_dev->data->nb_tx_queues;
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	if (fc_conf == NULL)
		return -EINVAL;

	switch (fc_conf->mode) {
	case RTE_ETH_FC_FULL:
		dev->tx_pause = 1;
		dev->rx_pause = 1;
		break;
	case RTE_ETH_FC_RX_PAUSE:
		dev->tx_pause = 0;
		dev->rx_pause = 1;
		break;
	case RTE_ETH_FC_TX_PAUSE:
		dev->tx_pause = 1;
		dev->rx_pause = 0;
		break;
	case RTE_ETH_FC_NONE:
		dev->tx_pause = 0;
		dev->rx_pause = 0;
		break;
	default:
		return -EINVAL;
	}

	dev->pause_autoneg = fc_conf->autoneg ? 1 : 0;
	dev->pause_time = fc_conf->pause_time ? fc_conf->pause_time : 0xffff;
	dev->fc_high_water = fc_conf->high_water;
	dev->fc_low_water = fc_conf->low_water;
	dev->flow_ctrl_cfg_set = 1;
	/* Link flow control supersedes any prior PFC configuration. */
	dev->pfc_queue_cfg_set = 0;
	memset(dev->pfc_rxq, 0, sizeof(dev->pfc_rxq));
	memset(dev->pfc_txq, 0, sizeof(dev->pfc_txq));

	if (eth_dev->data->dev_started)
		xgmac_flow_ctrl_apply(dev, nb_tx_queues, nb_rx_queues);

	return 0;
}

static int
xgmac_priority_flow_ctrl_queue_info_get(struct rte_eth_dev *eth_dev,
					struct rte_eth_pfc_queue_info *pfc_info)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	if (!pfc_info)
		return -EINVAL;

	pfc_info->tc_max = dev->hw_feat.tc_cnt;
	pfc_info->mode_capa = RTE_ETH_FC_FULL;
	return 0;
}

static int
xgmac_priority_flow_ctrl_queue_config(struct rte_eth_dev *eth_dev,
				      struct rte_eth_pfc_queue_conf *pfc_conf)
{
	uint16_t nb_rx_queues = eth_dev->data->nb_rx_queues;
	uint16_t nb_tx_queues = eth_dev->data->nb_tx_queues;
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint8_t tc_max, tc;
	uint16_t qid;
	bool en;

	if (!pfc_conf)
		return -EINVAL;

	/* PFC and link flow control cannot coexist. */
	if (dev->flow_ctrl_cfg_set && !dev->pfc_queue_cfg_set &&
	    (dev->tx_pause || dev->rx_pause)) {
		XGMAC_LOG(ERR, "disable link flow control before configuring PFC");
		return -ENOTSUP;
	}

	tc_max = dev->hw_feat.tc_cnt;

	/* Validate and update TX-pause direction (rx_qid/tc). */
	qid = pfc_conf->tx_pause.rx_qid;
	tc  = pfc_conf->tx_pause.tc;
	if (qid >= nb_rx_queues || qid >= dev->hw_feat.rx_q_cnt ||
	    tc >= tc_max)
		return -EINVAL;
	en = (pfc_conf->mode == RTE_ETH_FC_FULL) ||
	     (pfc_conf->mode == RTE_ETH_FC_TX_PAUSE);
	dev->pfc_rxq[qid].enabled = en ? 1 : 0;
	dev->pfc_rxq[qid].tc = tc;
	dev->pfc_rxq[qid].pause_time = pfc_conf->tx_pause.pause_time ?
				       pfc_conf->tx_pause.pause_time : 0xffff;

	/* Validate and update RX-pause direction (tx_qid/tc). */
	qid = pfc_conf->rx_pause.tx_qid;
	tc  = pfc_conf->rx_pause.tc;
	if (qid >= nb_tx_queues || qid >= dev->hw_feat.tx_q_cnt ||
	    tc >= tc_max)
		return -EINVAL;
	en = (pfc_conf->mode == RTE_ETH_FC_FULL) ||
	     (pfc_conf->mode == RTE_ETH_FC_RX_PAUSE);
	dev->pfc_txq[qid].enabled = en ? 1 : 0;
	dev->pfc_txq[qid].tc = tc;

	dev->pfc_queue_cfg_set = 1;

	if (eth_dev->data->dev_started)
		xgmac_pfc_queue_apply(dev, nb_tx_queues, nb_rx_queues);

	return 0;
}

static int
xgmac_vlan_offload_set(struct rte_eth_dev *eth_dev, int mask)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint64_t offloads = eth_dev->data->dev_conf.rxmode.offloads;

	if (mask & (RTE_ETH_VLAN_STRIP_MASK | RTE_ETH_QINQ_STRIP_MASK)) {
		if ((offloads & RTE_ETH_RX_OFFLOAD_QINQ_STRIP) && !dev->hw_feat.dvlan) {
			XGMAC_LOG(ERR, "QinQ strip requires Double VLAN support");
			return -ENOTSUP;
		}

		xgmac_vlan_strip_cfg(dev);

		if (eth_dev->data->dev_started)
			xgmac_rx_offload_update(eth_dev);
	}

	return xgmac_tx_offload_update(eth_dev);
}

static int
xgmac_vlan_tpid_set(struct rte_eth_dev *eth_dev, enum rte_vlan_type type, uint16_t tpid)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint8_t old_outer_svlan, old_inner_svlan;
	uint16_t old_outer_tpid, old_inner_tpid;

	old_outer_svlan = dev->vlan_outer_svlan;
	old_inner_svlan = dev->vlan_inner_svlan;
	old_outer_tpid = dev->vlan_outer_tpid;
	old_inner_tpid = dev->vlan_inner_tpid;

	switch (type) {
	case RTE_ETH_VLAN_TYPE_OUTER:
		dev->vlan_outer_tpid = tpid;
		dev->vlan_outer_svlan = xgmac_vlan_type_is_svlan(tpid);
		break;
	case RTE_ETH_VLAN_TYPE_INNER:
		if (!dev->hw_feat.dvlan)
			return -ENOTSUP;
		dev->vlan_inner_tpid = tpid;
		dev->vlan_inner_svlan = xgmac_vlan_type_is_svlan(tpid);
		break;
	default:
		return -EINVAL;
	}

	if (!xgmac_vlan_type_combo_valid(dev)) {
		dev->vlan_outer_svlan = old_outer_svlan;
		dev->vlan_inner_svlan = old_inner_svlan;
		dev->vlan_outer_tpid = old_outer_tpid;
		dev->vlan_inner_tpid = old_inner_tpid;
		return -EINVAL;
	}
	xgmac_vlan_insert_cfg(dev);

	return 0;
}

static int
xgmac_timesync_enable(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	int ret;

	if (!dev->hw_feat.ptp)
		return -ENOTSUP;

	dev->timesync_enable = 1;

	/* TSENALL mode (offload-timestamp) is a superset; skip re-init. */
	if (dev->timestamp_enable)
		return 0;

	ret = xgmac_timestamp_configure(dev);
	if (ret)
		return ret;

	if (eth_dev->data->dev_started)
		xgmac_rx_offload_update(eth_dev);

	return 0;
}

static int
xgmac_timesync_disable(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	dev->timesync_enable = 0;

	if (!dev->timestamp_enable) {
		xgmac_timestamp_disable(dev);
		if (eth_dev->data->dev_started)
			xgmac_rx_offload_update(eth_dev);
	}

	return 0;
}

static int
xgmac_timesync_read_rx_timestamp(struct rte_eth_dev *eth_dev, struct timespec *timestamp,
				 uint32_t flags __rte_unused)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint64_t tstamp;

	tstamp = rte_atomic_exchange_explicit(&dev->rx_tstamp, 0, rte_memory_order_relaxed);
	if (tstamp == 0)
		return -EINVAL;

	*timestamp = rte_ns_to_timespec(tstamp);
	return 0;
}

static int
xgmac_timesync_read_tx_timestamp(struct rte_eth_dev *eth_dev, struct timespec *timestamp)
{
	uint32_t sec, nsec;
	int ret;

	ret = xgmac_timestamp_read_tx(eth_dev->data->dev_private, &sec, &nsec);
	if (ret == 0) {
		timestamp->tv_sec = sec;
		timestamp->tv_nsec = nsec;
	}
	return ret;
}

static int
xgmac_timesync_adjust_time(struct rte_eth_dev *eth_dev, int64_t delta)
{
	return xgmac_timestamp_adjust_time(eth_dev->data->dev_private, delta);
}

static int
xgmac_timesync_adjust_freq(struct rte_eth_dev *eth_dev, int64_t ppm)
{
	return xgmac_timestamp_adjust_freq(eth_dev->data->dev_private, ppm);
}

static int
xgmac_timesync_read_time(struct rte_eth_dev *eth_dev, struct timespec *ts)
{
	uint32_t sec, nsec;

	xgmac_timestamp_read_time(eth_dev->data->dev_private, &sec, &nsec);
	ts->tv_sec = sec;
	ts->tv_nsec = nsec;
	return 0;
}

static int
xgmac_timesync_write_time(struct rte_eth_dev *eth_dev, const struct timespec *ts)
{
	return xgmac_timestamp_write_time(eth_dev->data->dev_private, (uint32_t)ts->tv_sec,
					  (uint32_t)ts->tv_nsec);
}

static int
xgmac_rx_descriptor_status_op(void *rxq, uint16_t offset)
{
	return xgmac_rx_descriptor_status(rxq, offset);
}

static int
xgmac_get_monitor_addr_op(void *rxq, struct rte_power_monitor_cond *pmc)
{
	return xgmac_get_monitor_addr(rxq, pmc);
}

static int
xgmac_fw_version_get(struct rte_eth_dev *eth_dev, char *buf, size_t size)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint32_t v = dev->hw_feat.version;
	int n;

	if (buf == NULL && size > 0)
		return -EINVAL;

	n = snprintf(buf, size, "snps=0x%02x dev=0x%02x user=0x%02x",
		     (unsigned int)XGMAC_FIELD_GET(XGMAC_VERSION_SNPSVER, v),
		     (unsigned int)XGMAC_FIELD_GET(XGMAC_VERSION_DEVID, v),
		     (unsigned int)XGMAC_FIELD_GET(XGMAC_VERSION_USERVER, v));
	if (n < 0)
		return -EINVAL;
	if ((size_t)n >= size)
		return n + 1;

	return 0;
}

static int
xgmac_get_reg(struct rte_eth_dev *eth_dev, struct rte_dev_reg_info *regs)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint32_t total;

	if (regs == NULL)
		return -EINVAL;

	total = xgmac_regs_count(dev);
	regs->version = dev->hw_feat.version;
	regs->width = sizeof(uint32_t);

	if (regs->data == NULL) {
		regs->length = total;
		return 0;
	}

	if (regs->length && regs->length < total)
		return -ENOSPC;
	regs->length = total;

	xgmac_regs_dump(dev, regs->data);
	return 0;
}

static const uint32_t *
xgmac_dev_supported_ptypes_get(struct rte_eth_dev *dev __rte_unused,
			       size_t *no_of_elements)
{
	static const uint32_t ptypes[] = {
		RTE_PTYPE_L2_ETHER,
		RTE_PTYPE_L3_IPV4,
		RTE_PTYPE_L3_IPV6,
		RTE_PTYPE_L4_TCP,
		RTE_PTYPE_L4_UDP,
		RTE_PTYPE_L4_ICMP,
		RTE_PTYPE_L4_IGMP,
	};

	*no_of_elements = RTE_DIM(ptypes);
	return ptypes;
}

static const struct eth_dev_ops xgmac_eth_dev_ops = {
	.dev_configure = xgmac_dev_configure,
	.dev_start = xgmac_dev_start,
	.dev_stop = xgmac_dev_stop,
	.dev_close = xgmac_dev_close,
	.dev_infos_get = xgmac_dev_infos_get,
	.dev_supported_ptypes_get = xgmac_dev_supported_ptypes_get,
	.dev_set_link_up = xgmac_dev_set_link_up,
	.dev_set_link_down = xgmac_dev_set_link_down,
	.link_update = xgmac_link_update,
	.stats_get = xgmac_stats_get,
	.stats_reset = xgmac_stats_reset,
	.xstats_get = xgmac_xstats_get,
	.xstats_get_names = xgmac_xstats_get_names,
	.xstats_reset = xgmac_xstats_reset,
	.mtu_set = xgmac_mtu_set,
	.mac_addr_set = xgmac_mac_addr_set,
	.mac_addr_add = xgmac_mac_addr_add,
	.mac_addr_remove = xgmac_mac_addr_remove,
	.set_mc_addr_list = xgmac_set_mc_addr_list,
	.promiscuous_enable = xgmac_promiscuous_enable,
	.promiscuous_disable = xgmac_promiscuous_disable,
	.allmulticast_enable = xgmac_allmulticast_enable,
	.allmulticast_disable = xgmac_allmulticast_disable,
	.flow_ctrl_get = xgmac_flow_ctrl_get,
	.flow_ctrl_set = xgmac_flow_ctrl_set,
	.priority_flow_ctrl_queue_info_get = xgmac_priority_flow_ctrl_queue_info_get,
	.priority_flow_ctrl_queue_config = xgmac_priority_flow_ctrl_queue_config,
	.vlan_offload_set = xgmac_vlan_offload_set,
	.vlan_tpid_set = xgmac_vlan_tpid_set,
	.rx_queue_setup = xgmac_rx_queue_setup,
	.rx_queue_release = xgmac_rx_queue_release,
	.rx_queue_start = xgmac_rx_queue_start,
	.rx_queue_stop = xgmac_rx_queue_stop,
	.tx_queue_setup = xgmac_tx_queue_setup,
	.tx_queue_release = xgmac_tx_queue_release,
	.tx_queue_start = xgmac_tx_queue_start,
	.tx_queue_stop = xgmac_tx_queue_stop,
	.tx_done_cleanup = xgmac_tx_done_cleanup,
	.rxq_info_get = xgmac_rxq_info_get,
	.txq_info_get = xgmac_txq_info_get,
	.get_reg = xgmac_get_reg,
	.fw_version_get = xgmac_fw_version_get,
	.get_monitor_addr = xgmac_get_monitor_addr_op,
	.reta_update = xgmac_dev_rss_reta_update,
	.reta_query = xgmac_dev_rss_reta_query,
	.rss_hash_update = xgmac_dev_rss_hash_update,
	.rss_hash_conf_get = xgmac_dev_rss_hash_conf_get,
	.get_dcb_info = xgmac_dev_get_dcb_info,
	.timesync_enable = xgmac_timesync_enable,
	.timesync_disable = xgmac_timesync_disable,
	.timesync_read_rx_timestamp = xgmac_timesync_read_rx_timestamp,
	.timesync_read_tx_timestamp = xgmac_timesync_read_tx_timestamp,
	.timesync_adjust_time = xgmac_timesync_adjust_time,
	.timesync_adjust_freq = xgmac_timesync_adjust_freq,
	.timesync_read_time = xgmac_timesync_read_time,
	.timesync_write_time = xgmac_timesync_write_time,
};

/* Check if platform device is Synopsys XGMAC by reading device tree compatible. */
static bool
xgmac_is_compatible(const char *dev_name)
{
	char path[PATH_MAX];
	char buf[256];
	FILE *f;
	char *p;

	snprintf(path, sizeof(path), "%s/%s/of_node/compatible",
		 PLATFORM_DEVICES_PATH, dev_name);

	f = fopen(path, "r");
	if (f == NULL)
		return false;

	if (fgets(buf, sizeof(buf), f) == NULL) {
		fclose(f);
		return false;
	}
	fclose(f);

	p = strchr(buf, '\n');
	if (p != NULL)
		*p = '\0';

	if (strstr(buf, "snps,dwcxgmac") != NULL ||
	    strstr(buf, "snps,dwxgmac2") != NULL ||
	    strstr(buf, "snps,dwxgmac") != NULL)
		return true;

	return false;
}

static int
xgmac_platform_probe(struct rte_platform_device *pdev)
{
	struct rte_eth_dev *eth_dev;
	struct xgmac_dev *dev;
	uint32_t ver;

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return -ENOTSUP;

	if (!xgmac_is_compatible(pdev->name)) {
		XGMAC_LOG(ERR, "device %s is not Synopsys XGMAC, skipping", pdev->name);
		return -ENODEV;
	}

	if (pdev->num_resource == 0 || pdev->resource[0].mem.addr == NULL) {
		XGMAC_LOG(ERR, "%s: no CSR resource", pdev->name);
		return -ENODEV;
	}

	eth_dev = rte_eth_dev_allocate(pdev->name);
	if (eth_dev == NULL) {
		XGMAC_LOG(ERR, "%s: rte_eth_dev_allocate failed", pdev->name);
		return -ENOMEM;
	}

	eth_dev->data->dev_private = rte_zmalloc(pdev->name, sizeof(*dev), 0);
	if (eth_dev->data->dev_private == NULL) {
		XGMAC_LOG(ERR, "%s: failed to allocate device priv", pdev->name);
		rte_eth_dev_release_port(eth_dev);
		return -ENOMEM;
	}

	dev = eth_dev->data->dev_private;
	dev->csr_base = pdev->resource[0].mem.addr;
	dev->csr_size = pdev->resource[0].mem.len;
	dev->pdev = pdev;
	dev->tx_offload_flags = XGMAC_TX_OFFLOAD_NONE;
	dev->vlan_outer_tpid = RTE_ETHER_TYPE_VLAN;
	dev->vlan_inner_tpid = RTE_ETHER_TYPE_VLAN;
	dev->vlan_outer_svlan = 0;
	dev->vlan_inner_svlan = 0;
	dev->pause_autoneg = 1;
	dev->tx_pause = 0;
	dev->rx_pause = 0;
	dev->flow_ctrl_cfg_set = 0;
	dev->pfc_queue_cfg_set = 0;
	dev->pause_time = 0xffff;
	dev->fc_high_water = 0;
	dev->fc_low_water = 0;

	xgmac_hw_features_get(dev);
	ver = dev->hw_feat.version;
	XGMAC_LOG(INFO, "%s: synopsys_id=0x%02x dev_id=0x%02x", pdev->name, ver & 0xff,
		  (ver >> 8) & 0xff);

	eth_dev->data->mac_addrs = rte_zmalloc(
		pdev->name, (dev->hw_feat.addn_mac + 1) * sizeof(struct rte_ether_addr), 0);
	if (eth_dev->data->mac_addrs == NULL) {
		XGMAC_LOG(ERR, "%s: failed to allocate mac_addrs", pdev->name);
		rte_free(eth_dev->data->dev_private);
		rte_eth_dev_release_port(eth_dev);
		return -ENOMEM;
	}

	xgmac_mac_addr_read(dev, 0, &eth_dev->data->mac_addrs[0]);

	eth_dev->device = &pdev->device;
	eth_dev->dev_ops = &xgmac_eth_dev_ops;
	eth_dev->rx_pkt_burst = xgmac_recv_pkts_no_offload;
	eth_dev->rx_descriptor_status = xgmac_rx_descriptor_status_op;

	xgmac_tx_offload_update(eth_dev);
	rte_eth_dev_probing_finish(eth_dev);

	XGMAC_LOG(INFO, "%s: probed", pdev->name);

	return 0;
}

static int
xgmac_platform_remove(struct rte_platform_device *pdev)
{
	struct rte_eth_dev *eth_dev;

	eth_dev = rte_eth_dev_allocated(pdev->name);
	if (eth_dev == NULL)
		return 0;

	xgmac_dev_close(eth_dev);

	rte_eth_dev_release_port(eth_dev);

	XGMAC_LOG(INFO, "%s: removed", pdev->name);

	return 0;
}

static struct rte_platform_driver xgmac_pmd_drv = {
	.driver = {
		.name = XGMAC_PMD_NAME,
	},
	.probe  = xgmac_platform_probe,
	.remove = xgmac_platform_remove,
};

RTE_PMD_REGISTER_PLATFORM(net_xgmac, xgmac_pmd_drv);
RTE_PMD_REGISTER_ALIAS(net_xgmac, vfio-platform);
