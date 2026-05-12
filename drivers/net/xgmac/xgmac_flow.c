/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_compat.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_flow_driver.h>
#include <rte_ip.h>
#include <rte_malloc.h>

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

static int
xgmac_flow_compile_ipv4(const struct rte_flow_item *item, struct xgmac_flow_pattern *p,
			enum xgmac_flow_stage stage, struct rte_flow_error *error)
{
	const rte_be16_t etype_v = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	const rte_be16_t etype_m = rte_cpu_to_be_16(0xFFFF);
	const struct rte_flow_item_ipv4 *spec = item->spec;
	const struct rte_flow_item_ipv4 *mask = item->mask;
	int ret;

	RTE_SET_USED(stage);

	/* Anchor the preceding L2 ether_type to IPv4. If ETH/VLAN already pinned it to a
	 * different value the overlap check in xgmac_flow_pattern_lay() will reject the rule.
	 */
	ret = xgmac_flow_pattern_lay(p, p->l3_base_off - sizeof(rte_be16_t), &etype_v, &etype_m,
				     sizeof(rte_be16_t), item, error);
	if (ret)
		return ret;

	if (spec == NULL && mask == NULL)
		return 0;

	if (spec == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_SPEC, item,
					  "IPv4 mask without spec");

	if (mask == NULL)
		mask = &rte_flow_item_ipv4_mask;

	/* L4 offset downstream assumes version=4, IHL=5 (i.e. version_ihl=0x45). */
	if (mask->hdr.version_ihl != 0) {
		const uint8_t expected = 0x45;

		if ((spec->hdr.version_ihl & mask->hdr.version_ihl) !=
		    (expected & mask->hdr.version_ihl))
			return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM_MASK,
						  item, "only IPv4 version=4, IHL=5 supported");

		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv4_hdr, version_ihl),
			&spec->hdr.version_ihl, &mask->hdr.version_ihl, sizeof(uint8_t), item,
			error);
		if (ret)
			return ret;
	}

	if (mask->hdr.total_length != 0 || mask->hdr.packet_id != 0 ||
	    mask->hdr.time_to_live != 0 || mask->hdr.hdr_checksum != 0)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM_MASK, item,
					  "matching IPv4 total_length/packet_id/ttl/hdr_checksum"
					  " not supported");

	if (mask->hdr.fragment_offset != 0) {
		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv4_hdr, fragment_offset),
			&spec->hdr.fragment_offset, &mask->hdr.fragment_offset, sizeof(rte_be16_t),
			item, error);
		if (ret)
			return ret;
	}

	if (mask->hdr.type_of_service != 0) {
		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv4_hdr, type_of_service),
			&spec->hdr.type_of_service, &mask->hdr.type_of_service, sizeof(uint8_t),
			item, error);
		if (ret)
			return ret;
	}

	if (mask->hdr.next_proto_id != 0) {
		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv4_hdr, next_proto_id),
			&spec->hdr.next_proto_id, &mask->hdr.next_proto_id, sizeof(uint8_t), item,
			error);
		if (ret)
			return ret;
	}

	if (mask->hdr.src_addr != 0) {
		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv4_hdr, src_addr),
			&spec->hdr.src_addr, &mask->hdr.src_addr, sizeof(rte_be32_t), item, error);
		if (ret)
			return ret;
	}

	if (mask->hdr.dst_addr != 0) {
		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv4_hdr, dst_addr),
			&spec->hdr.dst_addr, &mask->hdr.dst_addr, sizeof(rte_be32_t), item, error);
		if (ret)
			return ret;
	}

	return 0;
}

static int
xgmac_flow_compile_ipv6(const struct rte_flow_item *item, struct xgmac_flow_pattern *p,
			enum xgmac_flow_stage stage, struct rte_flow_error *error)
{
	const rte_be16_t etype_v = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);
	const rte_be16_t etype_m = rte_cpu_to_be_16(0xFFFF);
	const struct rte_flow_item_ipv6 *spec = item->spec;
	const struct rte_flow_item_ipv6 *mask = item->mask;
	int ret;

	RTE_SET_USED(stage);

	ret = xgmac_flow_pattern_lay(p, p->l3_base_off - sizeof(rte_be16_t), &etype_v, &etype_m,
				     sizeof(rte_be16_t), item, error);
	if (ret)
		return ret;

	if (spec == NULL && mask == NULL)
		return 0;

	if (spec == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_SPEC, item,
					  "IPv6 mask without spec");

	if (mask == NULL)
		mask = &rte_flow_item_ipv6_mask;

	if (mask->has_hop_ext || mask->has_route_ext || mask->has_frag_ext || mask->has_auth_ext ||
	    mask->has_esp_ext || mask->has_dest_ext || mask->has_mobil_ext || mask->has_hip_ext ||
	    mask->has_shim6_ext)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM, item,
					  "matching IPv6 extension headers not supported");

	if (mask->hdr.payload_len != 0 || mask->hdr.hop_limits != 0)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM_MASK, item,
					  "matching IPv6 payload_len/hop_limits not supported");

	/* vtc_flow holds [version:4 | traffic_class:8 | flow_label:20] in network order. */
	if (mask->hdr.vtc_flow != 0) {
		const rte_be32_t expected = rte_cpu_to_be_32(0x60000000); /* version=6 */
		const rte_be32_t version_mask = rte_cpu_to_be_32(0xf0000000);

		if ((spec->hdr.vtc_flow & mask->hdr.vtc_flow & version_mask) !=
		    (expected & mask->hdr.vtc_flow & version_mask))
			return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM_MASK,
						  item, "only IPv6 version=6 supported");

		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv6_hdr, vtc_flow),
			&spec->hdr.vtc_flow, &mask->hdr.vtc_flow, sizeof(rte_be32_t), item, error);
		if (ret)
			return ret;
	}

	if (mask->hdr.proto != 0) {
		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv6_hdr, proto), &spec->hdr.proto,
			&mask->hdr.proto, sizeof(uint8_t), item, error);
		if (ret)
			return ret;
	}

	if (!rte_ipv6_addr_is_unspec(&mask->hdr.src_addr)) {
		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv6_hdr, src_addr),
			&spec->hdr.src_addr, &mask->hdr.src_addr, RTE_IPV6_ADDR_SIZE, item, error);
		if (ret)
			return ret;
	}

	if (!rte_ipv6_addr_is_unspec(&mask->hdr.dst_addr)) {
		ret = xgmac_flow_pattern_lay(
			p, p->l3_base_off + offsetof(struct rte_ipv6_hdr, dst_addr),
			&spec->hdr.dst_addr, &mask->hdr.dst_addr, RTE_IPV6_ADDR_SIZE, item, error);
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
	{RTE_FLOW_ITEM_TYPE_ETH, XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2),
	 XGMAC_FLOW_STAGE_L2_VLAN_OUTER, xgmac_flow_compile_eth},
	{RTE_FLOW_ITEM_TYPE_VLAN, XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2_VLAN_OUTER),
	 XGMAC_FLOW_STAGE_L2_VLAN_INNER, xgmac_flow_compile_vlan},
	{RTE_FLOW_ITEM_TYPE_VLAN, XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2_VLAN_INNER),
	 XGMAC_FLOW_STAGE_L3, xgmac_flow_compile_vlan},
	{RTE_FLOW_ITEM_TYPE_IPV4,
	 XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2_VLAN_OUTER) |
		 XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2_VLAN_INNER) |
		 XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L3),
	 XGMAC_FLOW_STAGE_L4, xgmac_flow_compile_ipv4},
	{RTE_FLOW_ITEM_TYPE_IPV6,
	 XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2_VLAN_OUTER) |
		 XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L2_VLAN_INNER) |
		 XGMAC_FLOW_STAGE_BIT(XGMAC_FLOW_STAGE_L3),
	 XGMAC_FLOW_STAGE_L4, xgmac_flow_compile_ipv6},
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
xgmac_flow_compile_action(struct xgmac_dev *dev, const struct rte_flow_action actions[],
			  struct xgmac_frp_action *action, struct rte_flow_error *error)
{
	const struct rte_flow_action *a;
	bool terminal_set = false;

	memset(action, 0, sizeof(*action));

	if (actions == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION_NUM, NULL,
					  "NULL action list");

	for (a = actions; a->type != RTE_FLOW_ACTION_TYPE_END; a++) {
		if (a->type == RTE_FLOW_ACTION_TYPE_VOID)
			continue;

		if (terminal_set)
			return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION, a,
						  "multiple terminal actions");

		switch (a->type) {
		case RTE_FLOW_ACTION_TYPE_QUEUE: {
			const struct rte_flow_action_queue *q = a->conf;

			if (q == NULL)
				return rte_flow_error_set(error, EINVAL,
							  RTE_FLOW_ERROR_TYPE_ACTION_CONF, a,
							  "QUEUE conf is NULL");
			if (q->index >= dev->hw_feat.rx_ch_cnt)
				return rte_flow_error_set(error, EINVAL,
							  RTE_FLOW_ERROR_TYPE_ACTION_CONF, a,
							  "QUEUE index out of range");
			action->accept_frame = true;
			action->dma_ch_mask = (uint16_t)RTE_BIT32(q->index);
			terminal_set = true;
			break;
		}
		case RTE_FLOW_ACTION_TYPE_DROP:
			action->reject_frame = true;
			action->dma_ch_mask = 0;
			terminal_set = true;
			break;
		case RTE_FLOW_ACTION_TYPE_PASSTHRU:
			action->accept_frame = true;
			action->reject_frame = true;
			action->dma_ch_mask = 0;
			terminal_set = true;
			break;
		default:
			return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ACTION, a,
						  "action not supported");
		}
	}

	if (!terminal_set)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION_NUM, NULL,
					  "no terminal action");

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

static void
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
xgmac_flow_apply_no_rules_state(struct xgmac_dev *dev)
{
	/* Install drop-all rule if isolated, or disable the parser if not. */
	if (dev->flow_isolated) {
		static const uint8_t drop_match_data[1] = {0};
		static const uint8_t drop_match_mask[1] = {0};
		struct xgmac_frp_flow_rule isolate_drop_rule = {
			.byte_offset = 0,
			.match_value = drop_match_data,
			.match_mask = drop_match_mask,
			.match_size = 1,
			.action = {
					.reject_frame = true,
				  },
		};

		return xgmac_frp_program_rules(dev, &isolate_drop_rule, 1);
	}

	return xgmac_frp_enable(dev, false);
}

static int
xgmac_flow_reprogram_locked(struct xgmac_dev *dev, struct rte_flow_error *error)
{
	struct xgmac_frp_flow_rule *rules;
	struct xgmac_flow *f;
	uint16_t nb = 0;
	int ret;

	TAILQ_FOREACH (f, &dev->flow_list, next)
		nb++;

	if (nb == 0) {
		ret = xgmac_flow_apply_no_rules_state(dev);
		if (ret)
			return rte_flow_error_set(
				error, -ret, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
				dev->flow_isolated ? "failed to install isolated drop entry" :
						     "failed to disable FRP parser");
		return 0;
	}

	rules = rte_calloc("xgmac_flow_rules", nb, sizeof(*rules), 0);
	if (rules == NULL)
		return rte_flow_error_set(error, ENOMEM, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
					  "rule buffer allocation failed");

	nb = 0;
	TAILQ_FOREACH (f, &dev->flow_list, next) {
		rules[nb].byte_offset = f->byte_offset;
		rules[nb].match_value = f->buf;
		rules[nb].match_mask = f->mask;
		rules[nb].match_size = f->match_size;
		rules[nb].action = f->action;
		nb++;
	}

	ret = xgmac_frp_program_rules(dev, rules, nb);
	rte_free(rules);
	if (ret) {
		/* HW table may be partially written; fall back to no-rules state. */
		if (xgmac_flow_apply_no_rules_state(dev))
			XGMAC_LOG(ERR,
				  "FRP fail-safe failed; parser may be in an inconsistent state");
		return rte_flow_error_set(error, -ret, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
					  "FRP rule programming failed");
	}

	return 0;
}

static int
xgmac_flow_compile(struct xgmac_dev *dev, const struct rte_flow_attr *attr,
		   const struct rte_flow_item pattern[], const struct rte_flow_action actions[],
		   struct xgmac_flow_pattern *p, struct xgmac_frp_action *act,
		   struct rte_flow_error *error)
{
	int ret;

	ret = xgmac_flow_validate_attr(attr, error);
	if (ret)
		return ret;

	if (pattern == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_NUM, NULL,
					  "NULL pattern");

	ret = xgmac_flow_compile_pattern(pattern, p, error);
	if (ret)
		return ret;

	/* Patterns must fit inside the HW parse window. */
	if (p->match_size > dev->hw_feat.frp_parse_buf_size)
		return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM, NULL,
					  "match span exceeds FRP parse window");

	return xgmac_flow_compile_action(dev, actions, act, error);
}

static int
xgmac_flow_validate(struct rte_eth_dev *eth_dev, const struct rte_flow_attr *attr,
		    const struct rte_flow_item pattern[], const struct rte_flow_action actions[],
		    struct rte_flow_error *error)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_flow_pattern p;
	struct xgmac_frp_action act;

	return xgmac_flow_compile(dev, attr, pattern, actions, &p, &act, error);
}

static struct rte_flow *
xgmac_flow_create(struct rte_eth_dev *eth_dev, const struct rte_flow_attr *attr,
		  const struct rte_flow_item pattern[], const struct rte_flow_action actions[],
		  struct rte_flow_error *error)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_flow_pattern p;
	struct xgmac_frp_action act;
	struct xgmac_flow *flow;
	int ret;

	if (xgmac_flow_compile(dev, attr, pattern, actions, &p, &act, error))
		return NULL;

	flow = rte_zmalloc("xgmac_flow", sizeof(*flow), 0);
	if (flow == NULL) {
		rte_flow_error_set(error, ENOMEM, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
				   "flow allocation failed");
		return NULL;
	}

	flow->priority = attr->priority;
	flow->byte_offset = 0;
	flow->match_size = p.match_size;
	memcpy(flow->buf, p.buf, p.match_size);
	memcpy(flow->mask, p.mask, p.match_size);
	flow->action = act;

	rte_spinlock_lock(&dev->flow_lock);
	xgmac_flow_insert_sorted(dev, flow);

	ret = xgmac_flow_reprogram_locked(dev, error);
	if (ret) {
		struct rte_flow_error restore_err = {0};

		TAILQ_REMOVE(&dev->flow_list, flow, next);
		/* Restore the previous rules. */
		xgmac_flow_reprogram_locked(dev, &restore_err);
		rte_spinlock_unlock(&dev->flow_lock);
		rte_free(flow);
		return NULL;
	}
	rte_spinlock_unlock(&dev->flow_lock);

	return (struct rte_flow *)flow;
}

static int
xgmac_flow_destroy(struct rte_eth_dev *eth_dev, struct rte_flow *flow, struct rte_flow_error *error)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_flow *xf = (struct xgmac_flow *)flow;
	struct xgmac_flow *cur;
	int ret;

	if (xf == NULL)
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_HANDLE, NULL,
					  "NULL handle");

	rte_spinlock_lock(&dev->flow_lock);
	TAILQ_FOREACH (cur, &dev->flow_list, next) {
		if (cur == xf)
			break;
	}
	if (cur == NULL) {
		rte_spinlock_unlock(&dev->flow_lock);
		return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_HANDLE, flow,
					  "unknown flow handle");
	}

	TAILQ_REMOVE(&dev->flow_list, xf, next);
	ret = xgmac_flow_reprogram_locked(dev, error);
	rte_spinlock_unlock(&dev->flow_lock);

	rte_free(xf);
	return ret;
}

static int
xgmac_flow_flush(struct rte_eth_dev *eth_dev, struct rte_flow_error *error)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	struct xgmac_flow *flow;
	int ret;

	rte_spinlock_lock(&dev->flow_lock);

	ret = xgmac_flow_apply_no_rules_state(dev);
	if (ret) {
		rte_spinlock_unlock(&dev->flow_lock);
		return rte_flow_error_set(error, -ret, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
					  dev->flow_isolated ?
						  "failed to install isolated drop entry" :
						  "failed to disable FRP parser");
	}

	/* Don't wipe the drop-all rule if isolated. */
	if (!dev->flow_isolated) {
		ret = xgmac_frp_table_flush(dev);
		if (ret) {
			rte_spinlock_unlock(&dev->flow_lock);
			return rte_flow_error_set(error, -ret, RTE_FLOW_ERROR_TYPE_UNSPECIFIED,
						  NULL, "failed to flush FRP table");
		}
	}

	while ((flow = TAILQ_FIRST(&dev->flow_list)) != NULL) {
		TAILQ_REMOVE(&dev->flow_list, flow, next);
		rte_free(flow);
	}

	rte_spinlock_unlock(&dev->flow_lock);
	return 0;
}

static int
xgmac_flow_query(struct rte_eth_dev *eth_dev, struct rte_flow *flow,
		 const struct rte_flow_action *action, void *data, struct rte_flow_error *error)
{
	RTE_SET_USED(eth_dev);
	RTE_SET_USED(flow);
	RTE_SET_USED(action);
	RTE_SET_USED(data);

	/* No per-rule FRP counter support, so no query support. */
	return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_UNSPECIFIED, NULL,
				  "flow query not supported (no per-rule FRP counters)");
}

static int
xgmac_flow_isolate(struct rte_eth_dev *eth_dev, int set, struct rte_flow_error *error)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	const bool want = !!set;
	uint8_t saved;
	int ret;

	rte_spinlock_lock(&dev->flow_lock);

	if (dev->flow_isolated == want) {
		rte_spinlock_unlock(&dev->flow_lock);
		return 0;
	}

	/* Update flow_isolated; reprogram_locked re-derives the parser state. */
	saved = dev->flow_isolated;
	dev->flow_isolated = want;

	ret = xgmac_flow_reprogram_locked(dev, error);
	if (ret) {
		dev->flow_isolated = saved;
		rte_spinlock_unlock(&dev->flow_lock);
		return ret;
	}

	rte_spinlock_unlock(&dev->flow_lock);
	return 0;
}

void
xgmac_flow_restore(struct xgmac_dev *dev)
{
	struct rte_flow_error err = {0};
	int ret;

	if (!dev->hw_feat.frp)
		return;

	rte_spinlock_lock(&dev->flow_lock);
	ret = xgmac_flow_reprogram_locked(dev, &err);
	rte_spinlock_unlock(&dev->flow_lock);

	if (ret)
		XGMAC_LOG(WARNING, "failed to restore FRP flows on start: %s",
			  err.message ? err.message : "");
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
xgmac_flow_ops_get(struct rte_eth_dev *eth_dev, const struct rte_flow_ops **ops)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	/* Gate all rte_flow ops on FRP presence; non-FRP silicon advertises no flow support. */
	if (!dev->hw_feat.frp)
		return -ENOTSUP;

	*ops = &xgmac_flow_ops;
	return 0;
}
