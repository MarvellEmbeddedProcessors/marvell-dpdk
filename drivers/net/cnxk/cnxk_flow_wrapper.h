/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2020 Marvell.
 */
#ifndef __CNXK_FLOW_WRAPPER_H__
#define __CNXK_FLOW_WRAPPER_H__

#include <rte_flow_driver.h>

struct rte_flow *cnxk_flow_create_wrapper(struct rte_eth_dev *dev, const struct rte_flow_attr *attr,
					  const struct rte_flow_item pattern[],
					  const struct rte_flow_action actions[],
					  struct rte_flow_error *error);
int cnxk_flow_destroy_wrapper(struct rte_eth_dev *dev, struct rte_flow *flow,
			      struct rte_flow_error *error);

int cnxk_flow_info_get_wrapper(struct rte_eth_dev *dev, struct rte_flow_port_info *port_info,
			       struct rte_flow_queue_info *queue_info, struct rte_flow_error *err);

#define CNXK_NPC_COUNTERS_MAX 512

#endif /* __CNXK_FLOW_WRAPPER_H__ */
