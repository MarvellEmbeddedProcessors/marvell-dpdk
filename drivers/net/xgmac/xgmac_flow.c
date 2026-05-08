/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_compat.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_flow_driver.h>

#include "xgmac_ethdev.h"
#include "xgmac_flow.h"

enum xgmac_flow_stage {
	XGMAC_FLOW_STAGE_L2 = 0,	/* expect ETH               */
	XGMAC_FLOW_STAGE_L2_VLAN_OUTER,	/* ETH consumed             */
	XGMAC_FLOW_STAGE_L2_VLAN_INNER,	/* ETH + 1 VLAN consumed    */
	XGMAC_FLOW_STAGE_L3,		/* L3 only (post-QinQ etc.) */
	XGMAC_FLOW_STAGE_L4,
	XGMAC_FLOW_STAGE_DONE,
};

#define XGMAC_FLOW_STAGE_BIT(s) ((uint32_t)1 << (s))

struct xgmac_flow_pattern {
	uint8_t l3_base_off; /* 14 untagged, 18 single-tag, 22 QinQ */
	uint8_t match_size;
	uint8_t buf[XGMAC_FLOW_MATCH_BUF_SZ];
	uint8_t mask[XGMAC_FLOW_MATCH_BUF_SZ];
};

static int
xgmac_flow_pattern_lay(struct xgmac_flow_pattern *p, uint16_t byte_off, const void *value,
		       const void *mask, uint16_t len, const struct rte_flow_item *item,
		       struct rte_flow_error *error)
{
	uint32_t end = (uint32_t)byte_off + len;
	const uint8_t *v = value;
	const uint8_t *m = mask;
	uint16_t i;

	if (end > XGMAC_FLOW_MATCH_BUF_SZ)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM, item,
					  "match span exceeds driver buffer");

	for (i = 0; i < len; i++) {
		uint8_t prev_mask = p->mask[byte_off + i];
		uint8_t new_buf = v[i] & m[i];

		if (prev_mask == 0)
			continue;

		/* New item must be bitwise-identical to the old one, or it's an overlap error. */
		if (prev_mask != m[i] || p->buf[byte_off + i] != new_buf)
			return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM, item,
						  "pattern items overlap");
	}

	/* Commit the new value/mask. */
	for (i = 0; i < len; i++) {
		p->buf[byte_off + i] = v[i] & m[i];
		p->mask[byte_off + i] = m[i];
	}

	if (end > p->match_size)
		p->match_size = (uint8_t)end;

	return 0;
}

static int
xgmac_flow_compile_eth(const struct rte_flow_item *item, struct xgmac_flow_pattern *p,
		       enum xgmac_flow_stage stage, struct rte_flow_error *error)
{
	const struct rte_flow_item_eth *spec = item->spec;
	const struct rte_flow_item_eth *mask = item->mask;
	int ret;

	RTE_SET_USED(stage);

	p->l3_base_off = RTE_ETHER_HDR_LEN;

	if (spec == NULL && mask == NULL)
		return 0;

	if (spec == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_SPEC, item,
					  "ETH mask without spec");

	if (mask == NULL)
		mask = &rte_flow_item_eth_mask;

	ret = xgmac_flow_pattern_lay(p, 0, &spec->hdr.dst_addr, &mask->hdr.dst_addr,
				     RTE_ETHER_ADDR_LEN, item, error);
	if (ret)
		return ret;

	ret = xgmac_flow_pattern_lay(p, RTE_ETHER_ADDR_LEN, &spec->hdr.src_addr,
				     &mask->hdr.src_addr, RTE_ETHER_ADDR_LEN, item, error);
	if (ret)
		return ret;

	if (mask->has_vlan && spec->has_vlan) {
		const rte_be16_t tpid_v = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
		const rte_be16_t tpid_m = rte_cpu_to_be_16(0xFFFF);

		ret = xgmac_flow_pattern_lay(p, 2 * RTE_ETHER_ADDR_LEN, &tpid_v, &tpid_m,
					     sizeof(rte_be16_t), item, error);
		if (ret)
			return ret;

		p->l3_base_off = RTE_ETHER_HDR_LEN + 2 * sizeof(rte_be16_t);
	}

	if (mask->hdr.ether_type == 0)
		return 0;

	return xgmac_flow_pattern_lay(p, p->l3_base_off - sizeof(rte_be16_t),
				      &spec->hdr.ether_type, &mask->hdr.ether_type,
				      sizeof(rte_be16_t), item, error);
}

static int
xgmac_flow_compile_vlan(const struct rte_flow_item *item, struct xgmac_flow_pattern *p,
			enum xgmac_flow_stage stage, struct rte_flow_error *error)
{
	const struct rte_flow_item_vlan *spec = item->spec;
	const struct rte_flow_item_vlan *mask = item->mask;
	const rte_be16_t tpid_v = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
	const rte_be16_t tpid_m = rte_cpu_to_be_16(0xFFFF);
	const bool inner = (stage == XGMAC_FLOW_STAGE_L2_VLAN_INNER);
	/* outer: TPID@12 TCI@14 etype@16; inner: TPID@16 TCI@18 etype@20. */
	const uint16_t tpid_off = 2 * RTE_ETHER_ADDR_LEN +
				  (inner ? 2 * sizeof(rte_be16_t) : 0);
	const uint16_t tag_off = tpid_off + sizeof(rte_be16_t);
	const uint16_t etype_off = tag_off + sizeof(rte_be16_t);
	int ret;

	ret = xgmac_flow_pattern_lay(p, tpid_off, &tpid_v, &tpid_m,
				     sizeof(rte_be16_t), item, error);
	if (ret)
		return ret;

	p->l3_base_off = etype_off + sizeof(rte_be16_t);

	if (spec == NULL && mask == NULL)
		return 0;

	if (spec == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_SPEC, item,
					  "VLAN mask without spec");

	if (mask == NULL)
		mask = &rte_flow_item_vlan_mask;

	if (mask->has_more_vlan && spec->has_more_vlan) {
		if (inner)
			return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM,
						  item,
						  "has_more_vlan on inner VLAN not"
						  " supported (no triple-tag / QinQinQ)");

		ret = xgmac_flow_pattern_lay(p, etype_off, &tpid_v, &tpid_m,
					     sizeof(rte_be16_t), item, error);
		if (ret)
			return ret;

		p->l3_base_off = etype_off + 3 * sizeof(rte_be16_t);
	}

	if (mask->hdr.vlan_tci != 0) {
		ret = xgmac_flow_pattern_lay(p, tag_off, &spec->hdr.vlan_tci,
					     &mask->hdr.vlan_tci, sizeof(rte_be16_t),
					     item, error);
		if (ret)
			return ret;
	}

	if (mask->hdr.eth_proto != 0) {
		ret = xgmac_flow_pattern_lay(p, etype_off, &spec->hdr.eth_proto,
					     &mask->hdr.eth_proto, sizeof(rte_be16_t),
					     item, error);
		if (ret)
			return ret;
	}

	return 0;
}

/* allowed_stages picks which input stages dispatch here; next_stage is the post-compile stage. */
struct xgmac_flow_item_handler {
	enum rte_flow_item_type type;
	uint32_t allowed_stages;
	enum xgmac_flow_stage next_stage;
	int (*compile)(const struct rte_flow_item *item, struct xgmac_flow_pattern *p,
		       enum xgmac_flow_stage stage, struct rte_flow_error *error);
};

static const struct xgmac_flow_item_handler xgmac_flow_item_handlers[] = {
	{ RTE_FLOW_ITEM_TYPE_ETH,
	  XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2),
	  XGMAC_FLOW_STAGE_L2_VLAN_OUTER, xgmac_flow_compile_eth },
	{ RTE_FLOW_ITEM_TYPE_VLAN,
	  XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2_VLAN_OUTER),
	  XGMAC_FLOW_STAGE_L2_VLAN_INNER, xgmac_flow_compile_vlan },
	{ RTE_FLOW_ITEM_TYPE_VLAN,
	  XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2_VLAN_INNER),
	  XGMAC_FLOW_STAGE_L3, xgmac_flow_compile_vlan },
};

static const struct xgmac_flow_item_handler *
xgmac_flow_item_handler_find(enum rte_flow_item_type type, enum xgmac_flow_stage stage,
			     bool *type_known)
{
	const struct xgmac_flow_item_handler *match = NULL;
	size_t i;

	*type_known = false;
	for (i = 0; i < RTE_DIM(xgmac_flow_item_handlers); i++) {
		const struct xgmac_flow_item_handler *h = &xgmac_flow_item_handlers[i];

		if (h->type != type)
			continue;
		*type_known = true;
		if ((h->allowed_stages & XGMAC_FLOW_STAGE_BIT(stage)) != 0) {
			match = h;
			break;
		}
	}
	return match;
}

static int
xgmac_flow_compile_pattern(const struct rte_flow_item pattern[], struct xgmac_flow_pattern *p,
			   struct rte_flow_error *error)
{
	enum xgmac_flow_stage stage = XGMAC_FLOW_STAGE_L2;
	const struct rte_flow_item *item;
	int ret;

	memset(p, 0, sizeof(*p));

	for (item = pattern; item->type != RTE_FLOW_ITEM_TYPE_END; item++) {
		const struct xgmac_flow_item_handler *h;
		bool type_known;

		if (item->type == RTE_FLOW_ITEM_TYPE_VOID)
			continue;

		if (item->last != NULL)
			return rte_flow_error_set(error, ENOTSUP,
						  RTE_FLOW_ERROR_TYPE_ITEM_LAST, item,
						  "range matches not supported");

		h = xgmac_flow_item_handler_find(item->type, stage, &type_known);
		if (h == NULL) {
			if (!type_known)
				return rte_flow_error_set(error, ENOTSUP,
							  RTE_FLOW_ERROR_TYPE_ITEM, item,
							  "pattern item not supported");
			return rte_flow_error_set(error, EINVAL,
						  RTE_FLOW_ERROR_TYPE_ITEM, item,
						  "pattern items out of header order");
		}

		ret = h->compile(item, p, stage, error);
		if (ret)
			return ret;
		stage = h->next_stage;
	}

	return 0;
}

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
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_flow_pattern p;
	int ret;

	ret = xgmac_flow_validate_attr(attr, error);
	if (ret)
		return ret;

	if (pattern == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_NUM, NULL,
					  "NULL pattern");
	if (actions == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION_NUM, NULL,
					  "NULL action list");

	ret = xgmac_flow_compile_pattern(pattern, &p, error);
	if (ret)
		return ret;

	/* Patterns must fit inside the HW parse window. */
	if (p.match_size > dev->hw_feat.frp_parse_buf_size)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM, NULL,
					  "match span exceeds FRP parse window");

	/* Action compilation lands in the next stepped commit. */
	return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ACTION, NULL,
				  "actions not yet supported");
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
