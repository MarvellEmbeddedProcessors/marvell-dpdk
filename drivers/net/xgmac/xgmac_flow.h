/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#ifndef __XGMAC_FLOW_H__
#define __XGMAC_FLOW_H__

#include <sys/queue.h>

#include <stdint.h>

#include <ethdev_driver.h>

#include "xgmac_dev.h"

/* Maximum match buffer size for a flow. */
#define XGMAC_FLOW_MATCH_BUF_SZ 64

/* Maximum priority value allowed for an rte_flow. */
#define XGMAC_FLOW_MAX_PRIORITY 7

/*
 * Per-flow PMD state.
 *
 * One xgmac_flow corresponds to exactly one rte_flow handle and produces
 * exactly one xgmac_frp_flow_rule when the FRP IT is (re)programmed. The
 * priority + insert_seq pair gives a strict total order over all installed
 * flows so the compiled rule chain reflects rte_flow priority semantics.
 */
struct xgmac_flow {
	TAILQ_ENTRY(xgmac_flow) next;

	/* Lower values have higher priority. Same priority values are ordered by insert_seq. */
	uint8_t priority;
	uint64_t insert_seq;

	/* Match criteria for this flow. */
	uint16_t byte_offset;
	uint8_t match_size;
	uint8_t buf[XGMAC_FLOW_MATCH_BUF_SZ];
	uint8_t mask[XGMAC_FLOW_MATCH_BUF_SZ];

	/* Actions for this flow. */
	struct xgmac_frp_action action;
};

/* eth_dev_ops.flow_ops_get hook. */
int xgmac_flow_ops_get(struct rte_eth_dev *eth_dev, const struct rte_flow_ops **ops);

/* Replay the SW flow list onto the freshly-initialised FRP HW. */
void xgmac_flow_restore(struct xgmac_dev *dev);

#endif /* __XGMAC_FLOW_H__ */
