/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2025 Marvell.
 */

#ifndef _RTE_PMD_CNXK_EMDEV_H_
#define _RTE_PMD_CNXK_EMDEV_H_

#include <rte_ether.h>

#define CNXK_EMDEV_ATTR_FUNC_Q_MAP "func_q_map:"
#define CNXK_EMDEV_ATTR_NAME_LEN   20

#define RTE_PMD_EMDEV_MAX	16
#define RTE_PMD_EMDEV_FUNCS_MAX 128

/** Device status callback */
typedef int (*rte_pmd_cnxk_emdev_status_cb_t)(uint16_t emdev_id, uint16_t func_id, uint8_t status);

enum rte_pmd_emdev_type {
	EMDEV_TYPE_VIRTIO_NET = 1,
	EMDEV_TYPE_VIRTIO_CRYPTO = 2,
	EMDEV_TYPE_MAX,
};

struct rte_pmd_cnxk_func_q_map_attr {
	uint16_t func_id;
	uint16_t outb_qid;
	uint16_t qid;
};

struct rte_pmd_cnxk_emdev_info {
	uint16_t num_dev_funcs;
};

struct rte_pmd_cnxk_emdev_conf {
	uint16_t num_emdev_queues;
	uint16_t max_outb_queues;
	uint16_t num_funcs;
	enum rte_pmd_emdev_type emdev_type;

	rte_pmd_cnxk_emdev_status_cb_t status_cb;

	/* Default mempool */
	struct rte_mempool *default_mp;
};

struct rte_pmd_cnxk_emdev_q_conf {
	uint16_t nb_desc;
};

#endif /* _RTE_PMD_CNXK_EMDEV_H_ */
