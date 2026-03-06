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

#include "xgmac_ethdev.h"

#define XGMAC_PMD_NAME "net_xgmac"
#define PLATFORM_DEVICES_PATH "/sys/bus/platform/devices"

#define XGMAC_VERSION_REG 0x110

RTE_LOG_REGISTER_DEFAULT(xgmac_logtype, INFO);

static int
xgmac_dev_configure(struct rte_eth_dev *eth_dev __rte_unused)
{
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
xgmac_dev_infos_get(struct rte_eth_dev *eth_dev __rte_unused, struct rte_eth_dev_info *info)
{
	info->max_mac_addrs = 1;
	return 0;
}

static const struct eth_dev_ops xgmac_eth_dev_ops = {
	.dev_configure = xgmac_dev_configure,
	.dev_start = xgmac_dev_start,
	.dev_stop = xgmac_dev_stop,
	.dev_close = xgmac_dev_close,
	.dev_infos_get = xgmac_dev_infos_get,
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

	eth_dev->data->mac_addrs = rte_zmalloc(pdev->name, sizeof(struct rte_ether_addr), 0);
	if (eth_dev->data->mac_addrs == NULL) {
		XGMAC_LOG(ERR, "%s: failed to allocate mac_addrs", pdev->name);
		rte_free(eth_dev->data->dev_private);
		rte_eth_dev_release_port(eth_dev);
		return -ENOMEM;
	}

	ver = xgmac_rd(dev, XGMAC_VERSION_REG);
	XGMAC_LOG(INFO, "%s: synopsys_id=0x%02x dev_id=0x%02x",
		  pdev->name, ver & 0xff, (ver >> 8) & 0xff);

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
