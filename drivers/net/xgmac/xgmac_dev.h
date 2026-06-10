/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#ifndef __XGMAC_DEV_H__
#define __XGMAC_DEV_H__

#include <stdbool.h>

#include <rte_io.h>

struct xgmac_dev;

/* MAC core helpers. */
void xgmac_mac_init(struct xgmac_dev *dev, uint16_t nb_rx_queues);
void xgmac_mac_addr_read(struct xgmac_dev *dev, uint32_t index, struct rte_ether_addr *addr);
void xgmac_mac_addr_write(struct xgmac_dev *dev, uint32_t index, const struct rte_ether_addr *addr);
void xgmac_mac_addr_clear(struct xgmac_dev *dev, uint32_t index);
void xgmac_mac_promiscuous_set(struct xgmac_dev *dev, bool enable);
void xgmac_mac_allmulticast_set(struct xgmac_dev *dev, bool enable);
void xgmac_mac_mtu_set(struct xgmac_dev *dev, uint16_t mtu);
int xgmac_mc_hash_filter_set(struct xgmac_dev *dev, struct rte_ether_addr *mc_addr_set,
			     uint32_t nb_mc_addr);
void xgmac_hw_features_get(struct xgmac_dev *dev);

/* MTL block helpers. */
void xgmac_mtl_init(struct xgmac_dev *dev, uint16_t nb_tx_queues, uint16_t nb_rx_queues);

#endif /* __XGMAC_DEV_H__ */
