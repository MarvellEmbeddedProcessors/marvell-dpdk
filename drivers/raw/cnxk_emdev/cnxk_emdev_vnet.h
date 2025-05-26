/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2025 Marvell.
 */

#ifndef _CNXK_EMDEV_VIRTIO_NET_H_
#define _CNXK_EMDEV_VIRTIO_NET_H_

#include "cnxk_emdev.h"
#include "cnxk_emdev_virtio.h"
#include "roc_api.h"
#include "rte_pmd_cnxk_emdev.h"

#include "spec/virtio.h"
#include "spec/virtio_net.h"

#define VNET_DESC_ENTRY_SZ	   16UL
#define VNET_DESC_SZ(x)		   (WRAP_OFF(x) * VNET_DESC_ENTRY_SZ)
#define VNET_DESC_PTR_OFF(b, i, o) (uint64_t *)(((uintptr_t)b) + VNET_DESC_SZ(i) + (o))

typedef int (*cnxk_emdev_vnet_psw_dbl_fn_t)(void *queue, void *vnet_q, uint16_t index);
typedef int (*cnxk_emdev_vnet_dpi_compl_fn_t)(void *queue, void *vnet_q, uint16_t index);
typedef int (*cnxk_emdev_vnet_enq_fn_t)(void *queue, void *vnet_q, struct rte_mbuf **mbufs,
					uint16_t count);

extern cnxk_emdev_vnet_psw_dbl_fn_t cnxk_emdev_vnet_psw_dbl_fn[];
extern cnxk_emdev_vnet_dpi_compl_fn_t cnxk_emdev_vnet_dpi_compl_fn[];
extern cnxk_emdev_vnet_enq_fn_t cnxk_emdev_vnet_enq_fn[];

/* Emdev Vnet PSW doorbell Offloads */
#define EMDEV_VNET_PSW_DBL_OFFLOAD_NONE		 (0)
#define EMDEV_VNET_PSW_DBL_OFFLOAD_ENQ		 RTE_BIT64(0)
#define EMDEV_VNET_PSW_DBL_OFFLOAD_CTRL_DEQ	 RTE_BIT64(1)
#define EMDEV_VNET_PSW_DBL_OFFLOAD_DEQ		 RTE_BIT64(2)
#define EMDEV_VNET_PSW_DBL_OFFLOAD_DEQ_NOINORDER RTE_BIT64(3)
#define EMDEV_VNET_PSW_DBL_OFFLOAD_LAST		 RTE_BIT64(3)

#define DBL_ENQ_F	    EMDEV_VNET_PSW_DBL_OFFLOAD_ENQ
#define DBL_CTRL_F	    EMDEV_VNET_PSW_DBL_OFFLOAD_CTRL_DEQ
#define DBL_DEQ_F	    EMDEV_VNET_PSW_DBL_OFFLOAD_DEQ
#define DBL_DEQ_NOINORDER_F EMDEV_VNET_PSW_DBL_OFFLOAD_DEQ_NOINORDER

#define EMDEV_VNET_PSW_DBL_FASTPATH_MODES                                                          \
	D(none, EMDEV_VNET_PSW_DBL_OFFLOAD_NONE)                                                   \
	D(enq, DBL_ENQ_F)                                                                          \
	D(ctrl, DBL_CTRL_F)                                                                        \
	D(deq, DBL_DEQ_F)                                                                          \
	D(deq_noinorder, DBL_DEQ_F | DBL_DEQ_NOINORDER_F)

#define D(name, flags) int cnxk_emdev_vnet_psw_dbl_##name(void *q, void *vnet_q, uint16_t idx);

EMDEV_VNET_PSW_DBL_FASTPATH_MODES
#undef D

/* Emdev Vnet DPI Completion Offloads */
#define EMDEV_VNET_DPI_COMPL_OFFLOAD_NONE	   (0)
#define EMDEV_VNET_DPI_COMPL_OFFLOAD_ENQ	   RTE_BIT64(0)
#define EMDEV_VNET_DPI_COMPL_OFFLOAD_DEQ	   RTE_BIT64(1)
#define EMDEV_VNET_DPI_COMPL_OFFLOAD_DEQ_NOINORDER RTE_BIT64(2)
#define EMDEV_VNET_DPI_COMPL_OFFLOAD_LAST	   RTE_BIT64(2)

#define DPI_ENQ_F	    EMDEV_VNET_DPI_COMPL_OFFLOAD_ENQ
#define DPI_DEQ_F	    EMDEV_VNET_DPI_COMPL_OFFLOAD_DEQ
#define DPI_DEQ_NOINORDER_F EMDEV_VNET_DPI_COMPL_OFFLOAD_DEQ_NOINORDER

#define EMDEV_VNET_DPI_COMPL_FASTPATH_MODES                                                        \
	C(none, EMDEV_VNET_DPI_COMPL_OFFLOAD_NONE)                                                 \
	C(enq, DPI_ENQ_F)                                                                          \
	C(deq, DPI_DEQ_F)                                                                          \
	C(deq_noinorder, DPI_DEQ_F | DPI_DEQ_NOINORDER_F)

#define C(name, flags) int cnxk_emdev_vnet_dpi_compl_##name(void *q, void *vnet_q, uint16_t idx);

EMDEV_VNET_DPI_COMPL_FASTPATH_MODES
#undef C

/* Emdev Vnet Enqueue Offloads */
#define EMDEV_VNET_ENQ_OFFLOAD_NONE (0)
#define EMDEV_VNET_ENQ_OFFLOAD_CTRL RTE_BIT64(0)
#define EMDEV_VNET_ENQ_OFFLOAD_FF   RTE_BIT64(1)
#define EMDEV_VNET_ENQ_OFFLOAD_LAST RTE_BIT64(1)

#define E_CTRL_F EMDEV_VNET_ENQ_OFFLOAD_CTRL
#define E_FF_F	 EMDEV_VNET_ENQ_OFFLOAD_FF

#define EMDEV_VNET_ENQ_FASTPATH_MODES                                                              \
	E(none, EMDEV_VNET_ENQ_OFFLOAD_NONE)                                                       \
	E(ctrl, E_CTRL_F)                                                                          \
	E(ff, E_FF_F)                                                                              \
	E(ctrl_ff, (E_CTRL_F | E_FF_F))

#define E(name, flags)                                                                             \
	int cnxk_emdev_vnet_enq_##name(void *q, void *vnet_q, struct rte_mbuf **pkts, uint16_t num);

EMDEV_VNET_ENQ_FASTPATH_MODES
#undef E

int cnxk_emdev_vnet_init(struct cnxk_emdev_virtio_pfvf *pfvf, struct rte_pmd_cnxk_vnet_conf *conf);
int cnxk_emdev_vnet_cfg_read(struct cnxk_emdev_virtio_pfvf *pfvf, uint32_t offset, void *data,
			     uint8_t len);

#endif /* _CNXK_EMDEV_VIRTIO_NET_H_ */
