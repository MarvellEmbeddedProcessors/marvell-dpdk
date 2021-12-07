/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2022 Marvell.
 */

#ifndef _CN10K_MLDEV_OPS_H_
#define _CN10K_MLDEV_OPS_H_

#include <rte_mldev.h>

extern struct rte_mldev_ops cn10k_ml_ops;

int cn10k_ml_dev_config(struct rte_mldev *dev, struct rte_mldev_config *conf);

int cn10k_ml_dev_close(struct rte_mldev *dev);

#endif /* _CNXK_MLDEV_OPS_H_ */