/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 *
 * Synopsys DesignWare XGMAC 10G Ethernet PMD.
 */

#include <bus_platform_driver.h>
#include <ethdev_driver.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>

#include "xgmac_dev.h"
#include "xgmac_ethdev.h"
#include "xgmac_regs.h"

#define XGMAC_PMD_NAME "net_xgmac"
#define PLATFORM_DEVICES_PATH "/sys/bus/platform/devices"

RTE_LOG_REGISTER_DEFAULT(xgmac_logtype, INFO);

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

	return 0;
}

static int
xgmac_dev_start(struct rte_eth_dev *eth_dev __rte_unused)
{
	return -ENOTSUP;
}

static int
xgmac_dev_stop(struct rte_eth_dev *eth_dev __rte_unused)
{
	return 0;
}

static int
xgmac_dev_close(struct rte_eth_dev *eth_dev __rte_unused)
{
	return 0;
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
	info->rx_offload_capa = 0;
	info->tx_offload_capa = 0;

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
	.mtu_set = xgmac_mtu_set,
	.mac_addr_set = xgmac_mac_addr_set,
	.mac_addr_add = xgmac_mac_addr_add,
	.mac_addr_remove = xgmac_mac_addr_remove,
	.set_mc_addr_list = xgmac_set_mc_addr_list,
	.promiscuous_enable = xgmac_promiscuous_enable,
	.promiscuous_disable = xgmac_promiscuous_disable,
	.allmulticast_enable = xgmac_allmulticast_enable,
	.allmulticast_disable = xgmac_allmulticast_disable,
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
