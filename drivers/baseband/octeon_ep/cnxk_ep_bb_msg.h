/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2022 Marvell.
 */
#ifndef _CNXK_EP_BB_MSG_H_
#define _CNXK_EP_BB_MSG_H_

#include <rte_bbdev.h>

#define	OCTEON_EP_MAX_SG_ENTRIES	8
#define	OCTEON_EP_MAX_BURST_SIZE	64
#define	OCTEON_EP_MAX_CAPA_ENTRIES	8

/** Different command types supported by the device */
enum oct_bbdev_cmd_type {
	OTX_BBDEV_CMD_INFO_GET = 0x80,	/* info_get */
	OTX_BBDEV_CMD_DEV_CONFIG,	/* device config */
	OTX_BBDEV_CMD_QUE_SETUP,	/* queue setup */
	OTX_BBDEV_CMD_QUE_RELEASE,	/* queue release */
	OTX_BBDEV_CMD_DEV_START,	/* device start */
	OTX_BBDEV_CMD_DEV_STOP,		/* device stop */
};

/* Config command status values */
enum oct_bbdev_cmd_err {
	OTX_BBDEV_CMD_NO_ERR = 0,	/* No error */
	OTX_BBDEV_CMD_FAIL_Q0_SETUP,	/* Queue 0 setup failed */
	OTX_BBDEV_CMD_FAIL_ENQUE,	/* Command enqueue failed */
	OTX_BBDEV_CMD_FAIL_TMOUT,	/* Response timeout */
	OTX_BBDEV_CMD_FAIL_NOMBUF,	/* Command mbuf alloc failed */
};

/* Get device info message payload */
struct oct_bbdev_info {
	struct rte_bbdev_driver_info	rte_info;	/* rte lib structure, pmd <==> dev */
	/* Storage for pointer objects in drv_info */
	char				driver_name[32];/* pmd <==> dev */
	enum rte_cpu_flag_t		cpu_flag_reqs;	/* pmd <==> dev */
	struct rte_bbdev_op_cap		capabilities[OCTEON_EP_MAX_CAPA_ENTRIES+1];
							/* pmd <==> dev */
};
/* Device configure message payload */
struct oct_bbdev_config {
	uint16_t	num_queues;	/* pmd ==> dev */
};
/* Device queue setup message payload */
struct oct_bbdev_queue_setup {
	uint16_t	q_no;			/* pmd ==> dev */
	struct rte_bbdev_queue_conf q_conf;	/* pmd ==> dev */
};
/* Device queue release message payload */
struct oct_bbdev_queue_release {
	uint16_t	q_no;		/* pmd ==> dev */
};

union oct_bbdev_msg_type {
	enum rte_bbdev_op_type	op_type;
	enum oct_bbdev_cmd_type	cmd_type;
};

struct oct_bbdev_seg_data {
	rte_iova_t	data;		/* IOVA of segment mbuf buf_head */
	uint32_t	length;		/* Segment data_len */
};
struct oct_bbdev_op_sg_list {
	uint16_t			num_segs;	/* Number of segments */
	struct oct_bbdev_seg_data	seg_data[OCTEON_EP_MAX_SG_ENTRIES]; /* Translated addr */
};

struct oct_bbdev_op_turbo_enc {
	struct rte_bbdev_enc_op		op;		/* Operation payload input by library */
	struct oct_bbdev_op_sg_list	in_sg_list;	/* Input scatter-gather list */
	struct oct_bbdev_op_sg_list	out_sg_list;	/* Output scatter-gather list */
};

struct oct_bbdev_op_ldpc_enc {
	struct rte_bbdev_enc_op		op;		/* Operation payload input by library */
	struct oct_bbdev_op_sg_list	in_sg_list;	/* Input scatter-gather list */
	struct oct_bbdev_op_sg_list	out_sg_list;	/* Output scatter-gather list */
};

struct oct_bbdev_op_turbo_dec {
	struct rte_bbdev_dec_op		op;		/* Operation payload input by library */
	rte_iova_t			soft_out_buf;	/* Soft output buffer address */
	struct oct_bbdev_op_sg_list	in_sg_list;	/* Input scatter-gather list */
	struct oct_bbdev_op_sg_list	out_sg_list;	/* Output scatter-gather list */
};

struct oct_bbdev_op_ldpc_dec {
	struct rte_bbdev_dec_op		op;		/* Operation payload input by library */
	rte_iova_t			soft_out_buf;	/* Soft output address */
	rte_iova_t			harq_cmb_in_buf;/* Hard combined input address */
	rte_iova_t			harq_cmb_out_buf;/* Hard combined output address */
	struct oct_bbdev_op_sg_list	in_sg_list;	/* Input scatter-gather list */
	struct oct_bbdev_op_sg_list	out_sg_list;	/* Output scatter-gather list */
};

/* TX SDP message format for config/operation commands */
struct oct_bbdev_op_msg {
	/* Fake ethernet header to aid NIX parsing */
	uint8_t		rsvd[12];
	uint16_t	tpid;		/* 0x8100 */
	/* vf_id, q_no form vlan ID */
	uint8_t		vf_id;
	uint8_t		q_no;
	/* msg_type forms ether_type */
	uint8_t		msg_type;	/* cmd/op (union oct_bbdev_msg_type) */
	uint8_t		unused;

	/* Followed by bbdev config/operation payload */
	union {
		/* Config commands */
		struct {
			int	status;		/* Operation status, pmd <== dev */
			union {
				struct oct_bbdev_info		dev_info;	/* info_get */
				struct oct_bbdev_config		dev_config;	/* device config */
				struct oct_bbdev_queue_setup	queue_setup;	/* queue setup */
				struct oct_bbdev_queue_release	queue_release;	/* queue release */
				/* OTX_BBDEV_CMD_DEV_START/STOP have no args */
			};
		};
		/* Operational commands */
		struct {
			void	*op_ptr;	/* rte_bbdev input op ptr save location */
			union {
				struct oct_bbdev_op_turbo_enc	turbo_enc;	/* turbo enc */
				struct oct_bbdev_op_turbo_dec	turbo_dec;	/* turbo dec */
				struct oct_bbdev_op_ldpc_enc	ldpc_enc;	/* ldpc enc */
				struct oct_bbdev_op_ldpc_dec	ldpc_dec;	/* ldpc dec */
			};
		};
	};
};

#define MBUF_TO_OCT_MSG(mbuf)		rte_pktmbuf_mtod(mbuf, struct oct_bbdev_op_msg *)

#endif  /* _CNXK_EP_BB_MSG_H_ */
