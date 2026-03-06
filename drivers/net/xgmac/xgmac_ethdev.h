/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#ifndef __XGMAC_ETHDEV_H__
#define __XGMAC_ETHDEV_H__

#include <rte_io.h>
#include <rte_log.h>

extern int xgmac_logtype;
#define RTE_LOGTYPE_XGMAC xgmac_logtype

#define XGMAC_LOG(level, ...) RTE_LOG_LINE_PREFIX(level, XGMAC, "%s(): ", __func__, __VA_ARGS__)

struct xgmac_dev {
	void *csr_base;
	size_t csr_size;
};

static inline uint32_t
xgmac_rd(struct xgmac_dev *dev, uint32_t offset)
{
	return rte_read32((volatile uint8_t *)dev->csr_base + offset);
}

static inline void
xgmac_wr(struct xgmac_dev *dev, uint32_t offset, uint32_t val)
{
	rte_write32(val, (volatile uint8_t *)dev->csr_base + offset);
}

#endif /* __XGMAC_ETHDEV_H__ */
