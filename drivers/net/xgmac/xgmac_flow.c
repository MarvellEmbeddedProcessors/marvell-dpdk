/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <errno.h>
#include <stdint.h>

#include <rte_compat.h>
#include <rte_flow.h>
#include <rte_flow_driver.h>

#include "xgmac_ethdev.h"
#include "xgmac_flow.h"

static int
xgmac_flow_validate_attr(const struct rte_flow_attr *attr, struct rte_flow_error *error)
{
	if (attr == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ATTR, NULL,
					  "NULL attributes");

	if (attr->group != 0)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ATTR_GROUP, attr,
					  "groups are not supported");

	if (attr->egress)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ATTR_EGRESS, attr,
					  "egress is not supported");

	if (attr->transfer)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ATTR_TRANSFER, attr,
					  "transfer is not supported");

	if (!attr->ingress)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ATTR_INGRESS, attr,
					  "ingress must be set");

	if (attr->priority > XGMAC_FLOW_MAX_PRIORITY)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ATTR_PRIORITY, attr,
					  "priority exceeds driver maximum");

	return 0;
}

static __rte_unused void
xgmac_flow_insert_sorted(struct xgmac_dev *dev, struct xgmac_flow *flow)
{
	struct xgmac_flow *cur;

	flow->insert_seq = dev->flow_next_seq++;

	/* Insert flow into the list keeping (priority, insert_seq) sorted in ascending order. */
	TAILQ_FOREACH (cur, &dev->flow_list, next) {
		if (flow->priority < cur->priority)
			break;
		if (flow->priority == cur->priority && flow->insert_seq < cur->insert_seq)
			break;
	}

	if (cur != NULL)
		TAILQ_INSERT_BEFORE(cur, flow, next);
	else
		TAILQ_INSERT_TAIL(&dev->flow_list, flow, next);
}

static int
xgmac_flow_validate(struct rte_eth_dev *eth_dev, const struct rte_flow_attr *attr,
		    const struct rte_flow_item pattern[], const struct rte_flow_action actions[],
		    struct rte_flow_error *error)
{
	int ret;

	RTE_SET_USED(eth_dev);

	ret = xgmac_flow_validate_attr(attr, error);
	if (ret)
		return ret;

	if (pattern == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_NUM, NULL,
					  "NULL pattern");
	if (actions == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION_NUM, NULL,
					  "NULL action list");

	/* TODO: validate match_size <= XGMAC_FLOW_MATCH_BUF_SZ */
	return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM, NULL,
				  "pattern items not yet supported");
}

static struct rte_flow *
xgmac_flow_create(struct rte_eth_dev *eth_dev, const struct rte_flow_attr *attr,
		  const struct rte_flow_item pattern[], const struct rte_flow_action actions[],
		  struct rte_flow_error *error)
{
	RTE_SET_USED(eth_dev);
	RTE_SET_USED(attr);
	RTE_SET_USED(pattern);
	RTE_SET_USED(actions);

	rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
			   "flow create not yet implemented");
	return NULL;
}

static int
xgmac_flow_destroy(struct rte_eth_dev *eth_dev, struct rte_flow *flow, struct rte_flow_error *error)
{
	RTE_SET_USED(eth_dev);
	RTE_SET_USED(flow);

	return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_HANDLE, NULL,
				  "flow destroy not yet implemented");
}

static int
xgmac_flow_flush(struct rte_eth_dev *eth_dev, struct rte_flow_error *error)
{
	RTE_SET_USED(eth_dev);

	return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
				  "flow flush not yet implemented");
}

static int
xgmac_flow_query(struct rte_eth_dev *eth_dev, struct rte_flow *flow,
		 const struct rte_flow_action *action, void *data, struct rte_flow_error *error)
{
	RTE_SET_USED(eth_dev);
	RTE_SET_USED(flow);
	RTE_SET_USED(action);
	RTE_SET_USED(data);

	return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
				  "flow query not supported");
}

static int
xgmac_flow_isolate(struct rte_eth_dev *eth_dev, int set, struct rte_flow_error *error)
{
	RTE_SET_USED(eth_dev);
	RTE_SET_USED(set);

	return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
				  "isolate mode not supported");
}

static const struct rte_flow_ops xgmac_flow_ops = {
	.validate = xgmac_flow_validate,
	.create = xgmac_flow_create,
	.destroy = xgmac_flow_destroy,
	.flush = xgmac_flow_flush,
	.query = xgmac_flow_query,
	.isolate = xgmac_flow_isolate,
};

int
xgmac_flow_ops_get(struct rte_eth_dev *eth_dev __rte_unused, const struct rte_flow_ops **ops)
{
	*ops = &xgmac_flow_ops;
	return 0;
}
