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

	if (conf->rxmode.mq_mode != RTE_ETH_MQ_RX_NONE) {
		XGMAC_LOG(ERR, "MQ mode %u not supported", conf->rxmode.mq_mode);
		return -EINVAL;
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
		return ret;

	xgmac_mac_init(dev, nb_rx_queues);
	xgmac_mac_mtu_set(dev, eth_dev->data->mtu);
	xgmac_mtl_init(dev, nb_tx_queues, nb_rx_queues);

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

	return ret;
}

static int
xgmac_dev_stop(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint16_t q;

	if (!eth_dev->data->dev_started)
		return 0;

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
	xgmac_dma_stop(dev);
	eth_dev->data->dev_started = 0;

	return 0;
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

	if (rx_free_thresh >= nb_rx_desc || rx_free_thresh > XGMAC_DEFAULT_RX_FREE_THRESH ||
	    nb_rx_desc % rx_free_thresh != 0) {
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
xgmac_tx_queue_setup(struct rte_eth_dev *dev, uint16_t tx_queue_id,
		     uint16_t nb_tx_desc, unsigned int socket_id,
		     const struct rte_eth_txconf *tx_conf)
{
	struct xgmac_tx_queue *txq;
	const struct rte_memzone *mz;
	size_t size;

	if (nb_tx_desc < XGMAC_MIN_RING_DESC || nb_tx_desc > XGMAC_MAX_RING_DESC ||
	    !rte_is_power_of_2(nb_tx_desc))
		return -EINVAL;

	if (dev->data->dev_started)
		return -EBUSY;

	if (dev->data->tx_queues[tx_queue_id]) {
		xgmac_tx_queue_free(dev->data->tx_queues[tx_queue_id]);
		dev->data->tx_queues[tx_queue_id] = NULL;
		dev->data->tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;
	}
	txq = rte_zmalloc_socket("xgmac_txq", sizeof(*txq), RTE_CACHE_LINE_SIZE, socket_id);
	if (!txq)
		return -ENOMEM;

	txq->nb_desc = nb_tx_desc;
	txq->queue_id = tx_queue_id;
	txq->port_id = dev->data->port_id;
	txq->free_thresh = tx_conf->tx_free_thresh ?
		tx_conf->tx_free_thresh : XGMAC_DEFAULT_TX_FREE_THRESH;
	if (txq->free_thresh > txq->nb_desc)
		txq->free_thresh = (txq->nb_desc >> 1);
	if (txq->free_thresh == 0)
		txq->free_thresh = 1;
	txq->cur = 0;
	txq->dirty = 0;
	txq->offloads = tx_conf->offloads | dev->data->dev_conf.txmode.offloads;
	txq->deferred_start = tx_conf->tx_deferred_start;

	size = nb_tx_desc * sizeof(union xgmac_tx_desc);
	mz = xgmac_dma_zone_reserve_bounded(dev, "tx_ring", tx_queue_id, size,
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

	dev->data->tx_queues[tx_queue_id] = txq;
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
	info->rx_offload_capa = RTE_ETH_RX_OFFLOAD_SCATTER;
	info->tx_offload_capa = RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

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
		}
	}

	return 0;
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

static const struct eth_dev_ops xgmac_eth_dev_ops = {
	.dev_configure = xgmac_dev_configure,
	.dev_start = xgmac_dev_start,
	.dev_stop = xgmac_dev_stop,
	.dev_close = xgmac_dev_close,
	.dev_infos_get = xgmac_dev_infos_get,
	.dev_set_link_up = xgmac_dev_set_link_up,
	.dev_set_link_down = xgmac_dev_set_link_down,
	.link_update = xgmac_link_update,
	.stats_get = xgmac_stats_get,
	.stats_reset = xgmac_stats_reset,
	.mtu_set = xgmac_mtu_set,
	.mac_addr_set = xgmac_mac_addr_set,
	.mac_addr_add = xgmac_mac_addr_add,
	.mac_addr_remove = xgmac_mac_addr_remove,
	.set_mc_addr_list = xgmac_set_mc_addr_list,
	.promiscuous_enable = xgmac_promiscuous_enable,
	.promiscuous_disable = xgmac_promiscuous_disable,
	.allmulticast_enable = xgmac_allmulticast_enable,
	.allmulticast_disable = xgmac_allmulticast_disable,
	.rx_queue_setup = xgmac_rx_queue_setup,
	.rx_queue_release = xgmac_rx_queue_release,
	.rx_queue_start = xgmac_rx_queue_start,
	.rx_queue_stop = xgmac_rx_queue_stop,
	.tx_queue_setup = xgmac_tx_queue_setup,
	.tx_queue_release = xgmac_tx_queue_release,
	.tx_queue_start = xgmac_tx_queue_start,
	.tx_queue_stop = xgmac_tx_queue_stop,
	.rxq_info_get = xgmac_rxq_info_get,
	.txq_info_get = xgmac_txq_info_get,
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
