/* SPDX-License-Identifier: Marvell-Proprietary
 * Copyright(C) 2025 Marvell.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <rte_branch_prediction.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_graph_worker.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_per_lcore.h>
#include <rte_rcu_qsbr.h>
#include <rte_string_fns.h>
#include <rte_vect.h>
#include <rte_rawdev.h>
#include <rte_pmd_cnxk_emdev.h>
#include <spec/virtio.h>
#include <spec/virtio_net.h>

#include <cmdline_parse.h>
#include <cmdline_parse_etheraddr.h>

#include "l2_node.h"

/* Log type */
#define RTE_LOGTYPE_VIRTIO_L2FWD RTE_LOGTYPE_USER1

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024

#define DEFAULT_QUEUES_PER_PORT 1

#define MAX_ETHDEV_RX_PER_LCORE 128
#define MAX_VIRTIO_RX_PER_LCORE 128

#define MAX_LCORE_PARAMS 1024

#define NB_SOCKETS 8

#define MAX_VFS_PER_EPF 3

#define APP_INFO(fmt, args...) RTE_LOG(INFO, VIRTIO_L2FWD, fmt, ##args)

#define APP_INFO_NH(fmt, args...) rte_log(RTE_LOG_INFO, RTE_LOGTYPE_VIRTIO_L2FWD, fmt, ##args)

#define APP_ERR(fmt, args...) RTE_LOG(ERR, VIRTIO_L2FWD, fmt, ##args)

#define DRV_NAME_LEN 14

#define CQ_NOTIF_QID 0ul

struct lcore_ethdev_rx {
	uint16_t portid;
	char node_name[RTE_NODE_NAMESIZE];
	struct l2_ethdev_rx_node_ctx *ethdev_rx;
	/* Tx can either be ethdev or virtio */
	struct l2_ethdev_tx_node_ctx *ethdev_tx;
	struct l2_emdev_enq_node_ctx *emdev_enq;
	uint16_t emdev_qid;
};

struct lcore_emdev_deq {
	uint16_t emdev_id;
	char node_name[RTE_NODE_NAMESIZE];
	struct l2_emdev_deq_node_ctx *emdev_deq;
	uint16_t emdev_qid;
};

/* Lcore conf */
struct lcore_conf {
	/* Fast path accessed */
	uint16_t nb_emdev_deq;
	struct lcore_emdev_deq emdev_deq[MAX_VIRTIO_RX_PER_LCORE];
	uint16_t nb_ethdev_rx;
	struct lcore_ethdev_rx ethdev_rx[MAX_ETHDEV_RX_PER_LCORE];
	uint32_t weight;

	struct rte_graph *graph;
	char name[RTE_GRAPH_NAMESIZE];
	rte_graph_t graph_id;
	struct rte_rcu_qsbr *qs_v;
} __rte_cache_aligned;

static uint64_t lcore_eth_mask[RTE_MAX_ETHPORTS];
static uint64_t lcore_emdev_mask[RTE_RAWDEV_MAX_DEVS];

/* virtio_devid->eth_port */
struct l2fwd_map {
	uint16_t id;
	uint16_t emdev_id;
#define ETHDEV_NEXT 1
#define VIRTIO_NEXT 2
	uint8_t type;
};

/* Static global variables used within this file. */
static uint16_t nb_rxd = RX_DESC_DEFAULT;
static uint16_t nb_txd = TX_DESC_DEFAULT;

static struct lcore_conf lcore_conf[RTE_MAX_LCORE];
static uint16_t lcore_list_wt_sorted[RTE_MAX_LCORE];

/**< Ports set in promiscuous mode off by default. */
static int promiscuous_on;

static bool enable_l4_csum; /**< Enable IPv4 checksum offload feature */
static int disable_tx_mseg; /**< disable default ethdev Tx multi-seg offload */
static int per_port_pool; /**< Use separate buffer pools per port; disabled */
			  /**< by default */

static volatile bool force_quit;

static struct rte_ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];

/* Mask of enabled ports */
static uint64_t port_mask_ena;
static uint16_t nb_ethdevs;
static uint64_t emdev_mask_ena = 0x1; /**< Mask of enabled emdevs */
static uint16_t nb_emdevs = 1;
static uint16_t num_outb_queues = 17;

/* Pcap trace */
static char pcap_filename[RTE_GRAPH_PCAP_FILE_SZ];
static uint64_t packet_to_capture = 1024;
static int pcap_trace_enable;
static uint32_t pktmbuf_count = 16 * 1024;

static struct l2fwd_map virtio_map[RTE_RAWDEV_MAX_DEVS][RTE_PMD_EMDEV_FUNCS_MAX];
static struct l2fwd_map eth_map[RTE_MAX_ETHPORTS];

static struct rte_eth_dev_info eth_dev_info[RTE_MAX_ETHPORTS];
static struct rte_eth_conf eth_dev_conf[RTE_MAX_ETHPORTS];
static uint16_t eth_dev_q_count[RTE_MAX_ETHPORTS];

static const char *emdev_deq_edge_names[RTE_RAWDEV_MAX_DEVS + RTE_MAX_ETHPORTS];
static uint16_t nb_emdev_deq_edges;

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = RTE_ETH_MQ_RX_RSS,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = RTE_ETH_RSS_IP,
		},
	},
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
		.offloads = RTE_ETH_TX_OFFLOAD_MULTI_SEGS,
	},
};

static int stats_enable;
static int verbose_stats;

static uint32_t max_pkt_len;
static int pool_buf_len = RTE_MBUF_DEFAULT_BUF_SIZE;

static struct rte_mempool *e_pktmbuf_pool[RTE_MAX_ETHPORTS];
static struct rte_mempool *v_pktmbuf_pool[RTE_RAWDEV_MAX_DEVS];

static bool ethdev_cgx_loopback;

static bool
is_ethdev_enabled(uint16_t portid)
{
	return port_mask_ena & RTE_BIT64(portid);
}

static bool
is_emdev_enabled(uint16_t devid)
{
	return emdev_mask_ena & RTE_BIT64(devid);
}

static int
check_lcore_params(void)
{
	uint8_t lcore;
	uint16_t i;

	for (i = 0; i < RTE_MAX_ETHPORTS; ++i) {
		if (!is_ethdev_enabled(i))
			continue;

		for (lcore = 0; lcore < RTE_MAX_LCORE; lcore++) {
			if (!(RTE_BIT64(lcore) & lcore_eth_mask[i]))
				continue;

			if (!rte_lcore_is_enabled(lcore)) {
				APP_ERR("Error: lcore %hhu is not enabled in lcore mask\n", lcore);
				return -1;
			}

			if (lcore == rte_get_main_lcore()) {
				APP_ERR("Error: lcore %u is main lcore\n", lcore);
				return -1;
			}
		}
	}

	for (i = 0; i < RTE_RAWDEV_MAX_DEVS; i++) {
		if (!is_emdev_enabled(i))
			continue;

		for (lcore = 0; lcore < RTE_MAX_LCORE; lcore++) {
			if (!(RTE_BIT64(lcore) & lcore_emdev_mask[i]))
				continue;

			if (!rte_lcore_is_enabled(lcore)) {
				APP_ERR("Error: lcore %hhu is not enabled in lcore mask\n", lcore);
				return -1;
			}

			if (lcore == rte_get_main_lcore()) {
				APP_ERR("Error: lcore %u is main lcore\n", lcore);
				return -1;
			}
		}
	}
	return 0;
}

static int
check_port_config(void)
{
	uint16_t portid;

	for (portid = 0; portid < RTE_MAX_ETHPORTS; ++portid) {
		if (!is_ethdev_enabled(portid))
			continue;

		if (!rte_eth_dev_is_valid_port(portid)) {
			APP_INFO("Port %u is not present on the board\n", portid);
			return -1;
		}
	}

	return 0;
}

static int
init_lcore_ethdev_rx(void)
{
	uint16_t portid, nb_ethdev_rx;
	uint8_t lcore;

	for (portid = 0; portid < RTE_MAX_ETHPORTS; ++portid) {
		if (!is_ethdev_enabled(portid))
			continue;

		for (lcore = 0; lcore < RTE_MAX_LCORE; lcore++) {
			if (!(RTE_BIT64(lcore) & lcore_eth_mask[portid]))
				continue;

			nb_ethdev_rx = lcore_conf[lcore].nb_ethdev_rx;
			if (nb_ethdev_rx >= MAX_ETHDEV_RX_PER_LCORE) {
				APP_ERR("Error: too many ethdev rx (%u) for lcore: %u\n",
					(unsigned int)nb_ethdev_rx + 1, (unsigned int)lcore);
				return -1;
			}

			lcore_conf[lcore].ethdev_rx[nb_ethdev_rx].portid = portid;
			snprintf(lcore_conf[lcore].ethdev_rx[nb_ethdev_rx].node_name,
				 RTE_NODE_NAMESIZE, "l2_ethdev_rx-%u", portid);
			lcore_conf[lcore].nb_ethdev_rx++;
		}
	}

	/* Initialize lcore list */
	for (lcore = 0; lcore < RTE_MAX_LCORE; lcore++)
		lcore_list_wt_sorted[lcore] = lcore;

	return 0;
}

static int
init_lcore_emdev_deq(void)
{
	uint16_t lcore, nb_emdev_deq, emdev_id, i, edge_id;
	struct lcore_conf *qconf;
	char node_name[RTE_NODE_NAMESIZE];

	/* Initialize emdev deq edges */
	snprintf(node_name, RTE_NODE_NAMESIZE, "%s", "pkt_drop");
	emdev_deq_edge_names[0] = strdup(node_name);
	if (!emdev_deq_edge_names[0]) {
		APP_ERR("Error: failed to allocate memory for emdev deq edge name\n");
		return -1;
	}
	nb_emdev_deq_edges++;

	for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
		if (!is_ethdev_enabled(i))
			continue;

		edge_id = nb_emdev_deq_edges++;
		snprintf(node_name, RTE_NODE_NAMESIZE, "l2_ethdev_tx-%u", i);
		emdev_deq_edge_names[edge_id] = strdup(node_name);
		if (!emdev_deq_edge_names[edge_id]) {
			APP_ERR("Error: failed to allocate memory for emdev deq edge name\n");
			return -1;
		}
	}

	/* Equally distribute emdev queues among all the subscribed lcores */
	for (emdev_id = 0; emdev_id < RTE_RAWDEV_MAX_DEVS; emdev_id++) {
		if (!is_emdev_enabled(emdev_id))
			continue;

		for (lcore = 0; lcore < RTE_MAX_LCORE; lcore++) {
			if (!(RTE_BIT64(lcore) & lcore_emdev_mask[emdev_id]))
				continue;
			qconf = &lcore_conf[lcore];
			nb_emdev_deq = qconf->nb_emdev_deq;
			if (nb_emdev_deq >= MAX_VIRTIO_RX_PER_LCORE) {
				APP_ERR("Error: too many emdev deq (%u) for lcore: %u\n",
					(unsigned int)nb_emdev_deq + 1, (unsigned int)lcore);
				return -1;
			}
			qconf->emdev_deq[nb_emdev_deq].emdev_id = emdev_id;
			snprintf(qconf->emdev_deq[nb_emdev_deq].node_name, RTE_NODE_NAMESIZE,
				 "l2_emdev_deq-%d", emdev_id);
			qconf->nb_emdev_deq++;
		}

		edge_id = nb_emdev_deq_edges++;
		snprintf(node_name, RTE_NODE_NAMESIZE, "l2_emdev_enq-%d", emdev_id);
		emdev_deq_edge_names[edge_id] = strdup(node_name);
		if (!emdev_deq_edge_names[edge_id]) {
			APP_ERR("Error: failed to allocate memory for emdev deq edge name\n");
			return -1;
		}

	}

	return 0;
}

/* Display usage */
static void
print_usage(const char *prgname)
{
	fprintf(stderr,
		"%s [EAL options] --"
		" -p PORTMASK"
		" -e EMDEV_MASK"
		" [-P]"
		" [-s]"
		" [-l]"
		" [--l2fwd-map (port,dev)[,(port,dev)]]"
		" [--max-pkt-len PKTLEN]"
		" [--pool-buf-len PKTLEN]"
		" [--per-port-pool]"
		" [--disable-tx-mseg]"
		" [--num-pkt-cap]"
		" [--enable-l4-csum]"
		" [--num-outb-queues]\n\n"

		"  -p PORTMASK: Hexadecimal bitmask of ports to configure\n"
		"  -e EMDEVMASK: Hexadecimal bitmask of emdevs to configure\n"
		"  -P : Enable promiscuous mode\n"
		"  -s : Enable stats. Giving it multiple times makes stats verbose.\n"
		"  -l : Enable CGX loopback\n"
		"  --eth-config (port,lcore_mask): Ethdev rx lcore mapping\n"
		"           Default is half of the found lcores would be mapped to all ethdevs\n"
		"  --emdev-config (dev,lcore_mask)[,(dev,lcore_mask)] : emdev deq lcore mapping\n"
		"           Default is half of the found lcores would be mapped to all emdev devs\n"
		"  --l2fwd-map (eX,VY.Z)[,(A,B.C)] : Ethdev Virtio map\n"
		"           X is ethdev port, Y is emdev id, Z is emdev func id\n"
		"           Default is (e0,v0.0),(e1,v0.1)... i.e ethdev 0 is mapped to virtio emdev 0 pf\n"
		"           ethdev 1 is mapped to virtio emdev 0 vf 0, etc\n"
		"  --max-pkt-len PKTLEN: maximum packet length in decimal (64-9600)\n"
		"  --pool-buf-len PKTLEN: maximum pool buffer length in decimal (64-9600)\n"
		"  --per-port-pool: Use separate buffer pool per port\n"
		"  --disable-tx-mseg: Disable ethdev Tx multi-seg offload capability\n"
		"  --pcap-enable: Enables pcap capture\n"
		"  --pcap-num-cap NUMPKT: Number of packets to capture\n"
		"  --pcap-file-name NAME: Pcap file name\n"
		"  --enable-l4-csum: Enable IPv4 L4 checksum offload capability\n"
		"  --num-outb-queues: Number of emdev outbound queues\n",
		prgname);
}

static uint64_t
parse_num_pkt_cap(const char *num_pkt_cap)
{
	uint64_t num_pkt;
	char *end = NULL;

	/* Parse decimal string */
	num_pkt = strtoull(num_pkt_cap, &end, 10);
	if ((num_pkt_cap[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	if (num_pkt == 0)
		return 0;

	return num_pkt;
}

static int
parse_max_pkt_len(const char *pktlen)
{
	unsigned long len;
	char *end = NULL;

	/* Parse decimal string */
	len = strtoul(pktlen, &end, 10);
	if ((pktlen[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (len == 0)
		return -1;

	return len;
}

static uint64_t
parse_uint(const char *str)
{
	char *end = NULL;
	unsigned long val;

	/* Parse hexadecimal string */
	val = strtoul(str, &end, 0);
	if ((str[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	return val;
}

static int
parse_eth_config(const char *q_arg)
{
	enum fieldnames { FLD_PORT = 0, FLD_LCORE_MASK, _NUM_FLD };
	unsigned long int_fld[_NUM_FLD];
	const char *p, *p0 = q_arg;
	char *str_fld[_NUM_FLD];
	uint32_t size;
	char s[256];
	char *end;
	int i;

	while ((p = strchr(p0, '(')) != NULL) {
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		memcpy(s, p, size);
		s[size] = '\0';
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') != _NUM_FLD)
			return -1;
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i])
				return -1;
		}

		if (int_fld[FLD_PORT] >= RTE_MAX_ETHPORTS ||
		    int_fld[FLD_LCORE_MASK] >= RTE_BIT64(RTE_MAX_LCORE)) {
			APP_ERR("Invalid port/lcore mask\n");
			return -1;
		}

		lcore_eth_mask[int_fld[FLD_PORT]] = int_fld[FLD_LCORE_MASK];
	}

	return 0;
}

static int
parse_emdev_config(const char *q_arg)
{
	enum fieldnames { FLD_DEV = 0, FLD_LCORE_MASK, _NUM_FLD };
	unsigned long int_fld[_NUM_FLD];
	const char *p, *p0 = q_arg;
	char *str_fld[_NUM_FLD];
	uint32_t size;
	char s[256];
	char *end;
	int i;

	while ((p = strchr(p0, '(')) != NULL) {
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		memcpy(s, p, size);
		s[size] = '\0';
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') != _NUM_FLD)
			return -1;
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i])
				return -1;
		}

		if (int_fld[FLD_DEV] >= RTE_RAWDEV_MAX_DEVS ||
		    int_fld[FLD_LCORE_MASK] >= RTE_BIT64(RTE_MAX_LCORE)) {
			APP_ERR("Invalid emdev/lcore mask\n");
			return -1;
		}

		lcore_emdev_mask[int_fld[FLD_DEV]] = int_fld[FLD_LCORE_MASK];
	}

	return 0;
}

static int
parse_l2fwd_map_config(const char *q_arg)
{
	enum fieldnames { FLD_PORTA = 0, FLD_PORTB, _NUM_FLD };
	uint16_t emdev_id = 0, func_id = 0;
	uint16_t emdev_id2 = 0, func_id2 = 0;
	unsigned long int_fld[_NUM_FLD][2];
	uint16_t portid, portid2;
	const char *p, *p0 = q_arg;
	char *str_fld[_NUM_FLD];
	char *end;
	char s[256];
	char *p2;
	uint32_t size;
	int i;

	while ((p = strchr(p0, '(')) != NULL) {
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		memset(int_fld, 0, sizeof(int_fld));
		memcpy(s, p, size);
		s[size] = '\0';
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') != _NUM_FLD)
			return -1;
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			p2 = strchr(str_fld[i], '.');
			if (p2 != NULL) {
				*p2 = '\0';
				int_fld[i][0] = strtoul(str_fld[i] + 1, &end, 0);
				int_fld[i][1] = strtoul(p2 + 1, &end, 0);
			} else {
				int_fld[i][0] = strtoul(str_fld[i] + 1, &end, 0);
			}
			if (errno != 0 || end == str_fld[i])
				return -1;
		}


		if (*str_fld[FLD_PORTA] == 'v') {
			emdev_id = int_fld[FLD_PORTA][0];
			func_id = int_fld[FLD_PORTA][1];
			if (*str_fld[FLD_PORTB] == 'v') {
				emdev_id2 = int_fld[FLD_PORTB][0];
				func_id2 = int_fld[FLD_PORTB][1];

				virtio_map[emdev_id][func_id].id = func_id2;
				virtio_map[emdev_id][func_id].emdev_id = emdev_id2;
				virtio_map[emdev_id][func_id].type = VIRTIO_NEXT;

				virtio_map[emdev_id2][func_id2].id = func_id;
				virtio_map[emdev_id2][func_id2].emdev_id = emdev_id;
				virtio_map[emdev_id2][func_id2].type = VIRTIO_NEXT;
			} else if (*str_fld[FLD_PORTB] == 'e') {
				portid = int_fld[FLD_PORTB][0];
				virtio_map[emdev_id][func_id].id = int_fld[FLD_PORTB][0];
				virtio_map[emdev_id][func_id].type = ETHDEV_NEXT;

				eth_map[portid].id = func_id;
				eth_map[portid].emdev_id = emdev_id;
				eth_map[portid].type = VIRTIO_NEXT;
			} else {
				APP_ERR("Invalid port type, not 'v' or 'e'\n");
				return -1;
			}
		} else if (*str_fld[FLD_PORTA] == 'e') {
			portid = int_fld[FLD_PORTA][0];
			if (*str_fld[FLD_PORTB] == 'v') {
				emdev_id = int_fld[FLD_PORTB][0];
				func_id = int_fld[FLD_PORTB][1];
				eth_map[portid].id = func_id;
				eth_map[portid].emdev_id = emdev_id;
				eth_map[portid].type = VIRTIO_NEXT;

				virtio_map[emdev_id][func_id].id = portid;
				virtio_map[emdev_id][func_id].type = ETHDEV_NEXT;
			} else if (*str_fld[FLD_PORTB] == 'e') {
				portid2 = int_fld[FLD_PORTB][0];
				eth_map[portid].id = portid2;
				eth_map[portid].type = ETHDEV_NEXT;

				eth_map[portid2].id = portid;
				eth_map[portid2].type = ETHDEV_NEXT;
			} else {
				APP_ERR("Invalid port type, not 'v' or 'e'\n");
				return -1;
			}
		} else {
			APP_ERR("Invalid port type, not 'v' or 'e'\n");
			return -1;
		}
	}

	return 0;
}

#define MAX_JUMBO_PKT_LEN  9600
#define MEMPOOL_CACHE_SIZE 512

static const char short_options[] = "p:" /* portmask */
				    "e:" /* virt dev mask */
				    "d:" /* DMA flush threshold */
				    "P"  /* promiscuous */
				    "f"  /* Disable auto free */
				    "s"  /* stats enable */
				    "y:" /* Override DMA vfid */
				    "l"  /* Enable CGX loopback */
	;

#define CMD_LINE_OPT_ETH_CONFIG    "eth-config"
#define CMD_LINE_OPT_EMDEV_CONFIG "emdev-config"
#define CMD_LINE_OPT_L2FWD_MAP     "l2fwd-map"
#define CMD_LINE_OPT_MAX_PKT_LEN   "max-pkt-len"
#define CMD_LINE_OPT_MAX_BUF_LEN   "pool-buf-len"
#define CMD_LINE_OPT_PER_PORT_POOL "per-port-pool"
#define CMD_LINE_OPT_DIS_TX_MSEG   "disable-tx-mseg"
#define CMD_LINE_OPT_PCAP_ENABLE   "pcap-enable"
#define CMD_LINE_OPT_NUM_PKT_CAP   "pcap-num-cap"
#define CMD_LINE_OPT_PCAP_FILENAME "pcap-file-name"
#define CMD_LINE_OPT_ENA_L4_CSUM   "enable-l4-csum"
#define CMD_LINE_OPT_NUM_QUEUES    "num-outb-queues"
enum {
	/* Long options mapped to a short option */

	/* First long only option value must be >= 256, so that we won't
	 * conflict with short options
	 */
	CMD_LINE_OPT_MIN_NUM = 256,
	CMD_LINE_OPT_ETH_CONFIG_NUM,
	CMD_LINE_OPT_EMDEV_CONFIG_NUM,
	CMD_LINE_OPT_L2FWD_MAP_NUM,
	CMD_LINE_OPT_MAX_PKT_LEN_NUM,
	CMD_LINE_OPT_MAX_BUF_LEN_NUM,
	CMD_LINE_OPT_PARSE_PER_PORT_POOL,
	CMD_LINE_OPT_PARSE_DIS_TX_MSEG,
	CMD_LINE_OPT_PARSE_PCAP_ENABLE,
	CMD_LINE_OPT_PARSE_NUM_PKT_CAP,
	CMD_LINE_OPT_PCAP_FILENAME_CAP,
	CMD_LINE_OPT_PARSE_ENA_L4_CSUM,
	CMD_LINE_OPT_PARSE_NUM_QUEUES,
};

static const struct option lgopts[] = {
	{CMD_LINE_OPT_ETH_CONFIG, 1, 0, CMD_LINE_OPT_ETH_CONFIG_NUM},
	{CMD_LINE_OPT_EMDEV_CONFIG, 1, 0, CMD_LINE_OPT_EMDEV_CONFIG_NUM},
	{CMD_LINE_OPT_L2FWD_MAP, 1, 0, CMD_LINE_OPT_L2FWD_MAP_NUM},
	{CMD_LINE_OPT_MAX_PKT_LEN, 1, 0, CMD_LINE_OPT_MAX_PKT_LEN_NUM},
	{CMD_LINE_OPT_MAX_BUF_LEN, 1, 0, CMD_LINE_OPT_MAX_BUF_LEN_NUM},
	{CMD_LINE_OPT_PER_PORT_POOL, 0, 0, CMD_LINE_OPT_PARSE_PER_PORT_POOL},
	{CMD_LINE_OPT_DIS_TX_MSEG, 0, 0, CMD_LINE_OPT_PARSE_DIS_TX_MSEG},
	{CMD_LINE_OPT_PCAP_ENABLE, 0, 0, CMD_LINE_OPT_PARSE_PCAP_ENABLE},
	{CMD_LINE_OPT_NUM_PKT_CAP, 1, 0, CMD_LINE_OPT_PARSE_NUM_PKT_CAP},
	{CMD_LINE_OPT_PCAP_FILENAME, 1, 0, CMD_LINE_OPT_PCAP_FILENAME_CAP},
	{CMD_LINE_OPT_ENA_L4_CSUM, 0, 0, CMD_LINE_OPT_PARSE_ENA_L4_CSUM},
	{CMD_LINE_OPT_NUM_QUEUES, 1, 0, CMD_LINE_OPT_PARSE_NUM_QUEUES},
	{NULL, 0, 0, 0},
};

/* Parse the argument given in the command line of the application */
static int
parse_args(int argc, char **argv)
{
	uint16_t portid, j, func_id, emdev_id;
	uint64_t emdev_mask_dflt = 0;
	uint64_t eth_mask_dflt = 0;
	char *prgname = argv[0];
	char *str;
	int option_index;
	char **argvopt;
	uint8_t lcore;
	int opt, rc;
	int i;

	/* Setup l2fwd map to defaults */
	emdev_id = 0;
	func_id = 0;
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		eth_map[portid].type = VIRTIO_NEXT;
		eth_map[portid].id = func_id;
		eth_map[portid].emdev_id = emdev_id;

		virtio_map[emdev_id][func_id].type = ETHDEV_NEXT;
		virtio_map[emdev_id][func_id].id = func_id;

		func_id++;
		if (func_id > MAX_VFS_PER_EPF) {
			func_id = 0;
			emdev_id++;
		}
	}

	/* Setup lcore mask of ethdev and virtio dev to default
	 * One for main lcore and rest divided
	 * among ethdev and virtio.
	 */

	j = 0;
	lcore = 0;
	for (; lcore < RTE_MAX_LCORE; lcore++) {
		if ((j == (rte_lcore_count() - 1) / 2) && rte_lcore_count() > 2)
			break;
		if (!rte_lcore_is_enabled(lcore) || (lcore == rte_get_main_lcore()))
			continue;

		eth_mask_dflt |= RTE_BIT64(lcore);
		j++;
	}

	j = 0;
	if (rte_lcore_count() <= 2)
		lcore = 0;

	for (; lcore < RTE_MAX_LCORE; lcore++) {
		if ((j == (rte_lcore_count() - 1) / 2) && rte_lcore_count() > 2)
			break;
		if (!rte_lcore_is_enabled(lcore) || (lcore == rte_get_main_lcore()))
			continue;

		emdev_mask_dflt |= RTE_BIT64(lcore);
		j++;
	}

	argvopt = argv;

	/* Error or normal output strings. */
	while ((opt = getopt_long(argc, argvopt, short_options, lgopts, &option_index)) != EOF) {
		switch (opt) {
		/* Portmask */
		case 'p':
			str = optarg;
			port_mask_ena = parse_uint(str);
			nb_ethdevs = __builtin_popcountl(port_mask_ena);
			if (nb_ethdevs < 1 || nb_ethdevs > RTE_MAX_ETHPORTS) {
				APP_ERR("Invalid portmask\n");
				print_usage(prgname);
				return -1;
			}
			break;
		case 'e':
			str = optarg;
			emdev_mask_ena = parse_uint(str);
			nb_emdevs = __builtin_popcountl(emdev_mask_ena);
			if (nb_emdevs < 1 || nb_emdevs > RTE_RAWDEV_MAX_DEVS) {
				APP_ERR("Invalid emdev mask\n");
				print_usage(prgname);
				return -1;
			}
			break;
		case 'P':
			promiscuous_on = 1;
			break;
		case 's':
			if (stats_enable)
				verbose_stats++;
			else
				stats_enable = 1;
			break;
		case 'l':
			ethdev_cgx_loopback = true;
			APP_INFO("Ethdev CGX loopback enabled\n");
			break;

		/* Long options */
		case CMD_LINE_OPT_ETH_CONFIG_NUM:
			rc = parse_eth_config(optarg);
			if (rc) {
				APP_ERR("Invalid eth config\n");
				print_usage(prgname);
				return -1;
			}
			break;

		case CMD_LINE_OPT_EMDEV_CONFIG_NUM:
			rc = parse_emdev_config(optarg);
			if (rc) {
				APP_ERR("Invalid virt config\n");
				print_usage(prgname);
				return -1;
			}
			break;
		case CMD_LINE_OPT_L2FWD_MAP_NUM:
			rc = parse_l2fwd_map_config(optarg);
			if (rc) {
				APP_ERR("Invalid eth config\n");
				print_usage(prgname);
				return -1;
			}
			break;
		case CMD_LINE_OPT_MAX_PKT_LEN_NUM:
			max_pkt_len = parse_max_pkt_len(optarg);
			break;

		case CMD_LINE_OPT_MAX_BUF_LEN_NUM:
			pool_buf_len = parse_max_pkt_len(optarg);
			if (pool_buf_len == -1)
				pool_buf_len = RTE_MBUF_DEFAULT_BUF_SIZE;
			break;

		case CMD_LINE_OPT_PARSE_PER_PORT_POOL:
			APP_INFO("Per port buffer pool is enabled\n");
			per_port_pool = 1;
			break;

		case CMD_LINE_OPT_PARSE_DIS_TX_MSEG:
			APP_INFO("Ethdev Tx multi-seg offload is disabled\n");
			disable_tx_mseg = 1;
			break;

		case CMD_LINE_OPT_PARSE_PCAP_ENABLE:
			APP_INFO("Packet capture enabled\n");
			pcap_trace_enable = 1;
			break;

		case CMD_LINE_OPT_PARSE_NUM_PKT_CAP:
			packet_to_capture = parse_num_pkt_cap(optarg);
			APP_INFO("Number of packets to capture: %" PRIu64 "\n", packet_to_capture);
			break;

		case CMD_LINE_OPT_PCAP_FILENAME_CAP:
			rte_strlcpy(pcap_filename, optarg, sizeof(pcap_filename));
			APP_INFO("Pcap file name: %s\n", pcap_filename);
			break;

		case CMD_LINE_OPT_PARSE_ENA_L4_CSUM:
			APP_INFO("IPv4 Checksum offload feature is enabled\n");
			enable_l4_csum = true;
			break;

		case CMD_LINE_OPT_PARSE_NUM_QUEUES:
			APP_INFO("Number of maximum outbound queues\n");
			num_outb_queues = parse_uint(optarg);
			break;

		default:
			print_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind - 1] = prgname;
	rc = optind - 1;
	optind = 1; /* Reset getopt lib */

	if (!nb_ethdevs || !nb_emdevs) {
		APP_ERR("Need at least one port and emdev\n");
		return -1;
	}
	for (i = 0; i < RTE_MAX_ETHPORTS; i++)
		lcore_eth_mask[i] = eth_mask_dflt;

	for (i = 0; i < RTE_RAWDEV_MAX_DEVS; i++)
		lcore_emdev_mask[i] = emdev_mask_dflt;

	return rc;
}

static void
print_ethaddr(const char *name, const struct rte_ether_addr *eth_addr)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];

	rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, eth_addr);
	APP_INFO_NH("%s%s", name, buf);
}

static int
init_eth_mempool(uint16_t portid, uint32_t nb_mbuf)
{
	char s[64];

	if (e_pktmbuf_pool[portid] == NULL) {
		snprintf(s, sizeof(s), "mbuf_pool_e%d", portid);
		/* Create a pool with priv size of a cacheline */
		e_pktmbuf_pool[portid] =
			rte_pktmbuf_pool_create(s, nb_mbuf, MEMPOOL_CACHE_SIZE,
						RTE_CACHE_LINE_SIZE, pool_buf_len, 0);
		if (e_pktmbuf_pool[portid] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
		else
			APP_INFO("Allocated ethdev mbuf pool for portid=%d\n", portid);
	}

	return 0;
}

static int
init_emdev_mempool(uint16_t devid, uint32_t nb_mbuf)
{
	char s[64];

	if (v_pktmbuf_pool[devid] == NULL) {
		snprintf(s, sizeof(s), "mbuf_pool_v%d", devid);
		/* Create a pool with priv size of a cacheline */
		v_pktmbuf_pool[devid] =
			rte_pktmbuf_pool_create(s, nb_mbuf, MEMPOOL_CACHE_SIZE,
						RTE_CACHE_LINE_SIZE, pool_buf_len, 0);
		if (v_pktmbuf_pool[devid] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
		else
			APP_INFO("Allocated virtio_dev mbuf pool for devid=%d\n", devid);
	}
	return 0;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(void)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90  /* 9s (90 * 100ms) in total */
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	uint16_t portid;
	int rc;

	APP_INFO("\n");
	APP_INFO("Checking link status\n");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (force_quit)
				return;
			if (!is_ethdev_enabled(portid))
				continue;
			memset(&link, 0, sizeof(link));
			rc = rte_eth_link_get_nowait(portid, &link);
			if (rc < 0) {
				all_ports_up = 0;
				if (print_flag == 1)
					APP_ERR("Port %u link get failed: %s\n", portid,
						rte_strerror(-rc));
				continue;
			}
			/* Print link status if flag set */
			if (print_flag == 1) {
				rte_eth_link_to_str(link_status_text, sizeof(link_status_text),
						    &link);
				APP_INFO("Port %d %s\n", portid, link_status_text);
				continue;
			}
			/* Clear all_ports_up flag if any link down */
			if (link.link_status == RTE_ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* After finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* Set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			APP_INFO("Done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	APP_INFO("\n");
	if (signum == SIGINT || signum == SIGTERM) {
		APP_INFO("Signal %d received, preparing to exit...\n", signum);
		force_quit = true;
	}
}


static uint32_t
eth_dev_get_overhead_len(uint32_t max_rx_pktlen, uint16_t max_mtu)
{
	uint32_t overhead_len;

	if (max_mtu != UINT16_MAX && max_rx_pktlen > max_mtu)
		overhead_len = max_rx_pktlen - max_mtu;
	else
		overhead_len = RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN;

	return overhead_len;
}

static int
config_port_max_pkt_len(struct rte_eth_conf *conf, struct rte_eth_dev_info *dev_info)
{
	uint32_t overhead_len;

	if (max_pkt_len == 0)
		return 0;

	if (max_pkt_len < RTE_ETHER_MIN_LEN || max_pkt_len > MAX_JUMBO_PKT_LEN)
		return -1;

	overhead_len = eth_dev_get_overhead_len(dev_info->max_rx_pktlen, dev_info->max_mtu);
	conf->rxmode.mtu = max_pkt_len - overhead_len;

	if (conf->rxmode.mtu > RTE_ETHER_MTU)
		conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

	return 0;
}


static void
setup_mempools(void)
{
	uint32_t emdev_id;
	uint16_t portid;
	int rc;

	/* Initialize all ports. 8< */
	RTE_ETH_FOREACH_DEV(portid) {
		/* Skip ports that are not enabled */
		if (!is_ethdev_enabled(portid))
			continue;

		/* Init memory */
		if (!per_port_pool) {
			/* portid = 0; this is *not* signifying the first port,
			 * rather, it signifies that portid is ignored.
			 */
			rc = init_eth_mempool(0, pktmbuf_count);
		} else {
			rc = init_eth_mempool(portid, pktmbuf_count);
		}
		if (rc < 0)
			rte_exit(EXIT_FAILURE, "init_eth_mempool() failed\n");
	}

	for (emdev_id = 0; emdev_id < RTE_RAWDEV_MAX_DEVS; emdev_id++) {
		if (!is_emdev_enabled(emdev_id))
			continue;

		if (!per_port_pool) {
			/* portid = 0; this is *not* signifying the first port,
			 * rather, it signifies that portid is ignored.
			 */
			rc = init_emdev_mempool(0, pktmbuf_count);
		} else {
			rc = init_emdev_mempool(emdev_id, pktmbuf_count);
		}
		if (rc < 0)
			rte_exit(EXIT_FAILURE, "init_virtio_mempool() failed\n");
	}
}

static void
setup_eth_devices(void)
{
	struct rte_eth_rss_reta_entry64 reta_conf[4];
	struct rte_eth_conf local_port_conf;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;
	uint16_t queueid, i, portid;
	uint16_t nb_rx_queue;
	uint32_t nb_tx_queue;
	int rc;

	APP_INFO("\n");

	RTE_ETH_FOREACH_DEV(portid) {
		local_port_conf = port_conf;

		/* Skip ports that are not enabled */
		if (!is_ethdev_enabled(portid)) {
			APP_INFO("Skipping disabled port %d\n", portid);
			continue;
		}

		/* Init port */
		APP_INFO("Initializing port %d ...", portid);
		fflush(stdout);

		if (rte_eth_dev_info_get(portid, &dev_info))
			rte_exit(EXIT_FAILURE, "rte_eth_dev_info_get() failed for port %d\n",
				 portid);
		eth_dev_info[portid] = dev_info;

		/* Setup ethdev with max Rx, Tx queues */
		if (eth_map[portid].type == VIRTIO_NEXT)
			nb_rx_queue = DEFAULT_QUEUES_PER_PORT;
		else
			nb_rx_queue = num_outb_queues / 2;

		nb_tx_queue = nb_rx_queue;
		eth_dev_q_count[portid] = nb_rx_queue;

		APP_INFO_NH("Creating queues: nb_rxq=%d nb_txq=%u... ", nb_rx_queue, nb_tx_queue);

		rc = config_port_max_pkt_len(&local_port_conf, &dev_info);
		if (rc != 0)
			rte_exit(EXIT_FAILURE, "Invalid max packet length: %u (port %u)\n",
				 max_pkt_len, portid);

		if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
			local_port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

		if (disable_tx_mseg)
			local_port_conf.txmode.offloads &= ~RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

		local_port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
		if (local_port_conf.rx_adv_conf.rss_conf.rss_hf !=
		    port_conf.rx_adv_conf.rss_conf.rss_hf) {
			APP_INFO("Port %u modified RSS hash function based on "
				 "hardware support,"
				 "requested:%#" PRIx64 " configured:%#" PRIx64 "\n",
				 portid, port_conf.rx_adv_conf.rss_conf.rss_hf,
				 local_port_conf.rx_adv_conf.rss_conf.rss_hf);
		}

		/* Enable CGX loopback mode if needed */
		local_port_conf.lpbk_mode = !!ethdev_cgx_loopback;

		rc = rte_eth_dev_configure(portid, nb_rx_queue, nb_tx_queue, &local_port_conf);
		if (rc < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%d\n", rc,
				 portid);
		eth_dev_conf[portid] = local_port_conf;

		rc = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
		if (rc < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot adjust number of descriptors: err=%d, "
				 "port=%d\n",
				 rc, portid);

		rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
		print_ethaddr(" Address:", &ports_eth_addr[portid]);
		APP_INFO_NH("\n");

		/* Setup Tx queues */
		for (queueid = 0; queueid < nb_tx_queue; queueid++) {
			txconf = &dev_info.default_txconf;
			txconf->offloads = local_port_conf.txmode.offloads;

			rc = rte_eth_tx_queue_setup(portid, queueid, nb_txd, 0, txconf);
			if (rc < 0)
				rte_exit(EXIT_FAILURE,
					 "rte_eth_tx_queue_setup: err=%d, "
					 "port=%d\n",
					 rc, portid);
		}

		/* Setup RX queues */
		for (queueid = 0; queueid < nb_rx_queue; queueid++) {
			struct rte_eth_rxconf rxq_conf;

			rxq_conf = dev_info.default_rxconf;
			rxq_conf.offloads = port_conf.rxmode.offloads;
			if (!per_port_pool)
				rc = rte_eth_rx_queue_setup(portid, queueid, nb_rxd, 0, &rxq_conf,
							    e_pktmbuf_pool[0]);
			else
				rc = rte_eth_rx_queue_setup(portid, queueid, nb_rxd, 0, &rxq_conf,
							    e_pktmbuf_pool[portid]);
			if (rc < 0)
				rte_exit(EXIT_FAILURE,
					 "rte_eth_rx_queue_setup: err=%d, "
					 "port=%d\n",
					 rc, portid);
		}

		/* Setup all entries in RETA table to point to RQ 0.
		 * RETA table will get updated when number of queue count
		 * is available.
		 */
		if (dev_info.reta_size) {
			memset(reta_conf, 0, sizeof(reta_conf));
			for (i = 0; i < 4; i++)
				reta_conf[i].mask = UINT64_MAX;

			rc = rte_eth_dev_rss_reta_update(portid, reta_conf, dev_info.reta_size);
			if (rc < 0)
				rte_exit(EXIT_FAILURE,
					 "Failed to update reta table to RQ 0, rc=%d\n", rc);
		}

		/* Disable ptype extraction */
		rc = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL, 0);
		if (rc < 0)
			rte_exit(EXIT_FAILURE, "Failed to disable ptype parsing\n");
	}

	APP_INFO("\n");
	/* Dump L2FWD map */
	RTE_ETH_FOREACH_DEV(portid) {
		if (!is_ethdev_enabled(portid))
			continue;
		if (eth_map[portid].type == ETHDEV_NEXT)
			APP_INFO("L2FWD_MAP: ethdev_rx[%u] =====> ethdev_tx[%u] (lcores 0x%lX)\n",
				 portid, eth_map[portid].id, lcore_eth_mask[portid]);
		else
			APP_INFO(
				"L2FWD_MAP: ethdev_rx[%u] ======> virtiodev_tx[%u] (lcores 0x%lX)\n",
				portid, eth_map[portid].id, lcore_eth_mask[portid]);
	}
}

static void
release_eth_devices(void)
{
	uint16_t portid;
	int rc;

	/* Stop ports */
	RTE_ETH_FOREACH_DEV(portid) {
		if (!is_ethdev_enabled(portid))
			continue;
		APP_INFO("Closing port %d...", portid);
		rc = rte_eth_dev_stop(portid);
		if (rc != 0)
			APP_ERR("Failed to stop port %u: %s\n", portid, rte_strerror(-rc));
		rte_eth_dev_close(portid);
		APP_INFO_NH(" Done\n");
	}
}

int
main(int argc, char **argv)
{
	uint16_t portid;
	int rc;

	/* Init EAL */
	rc = rte_eal_init(argc, argv);
	if (rc < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
	argc -= rc;
	argv += rc;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Parse application arguments (after the EAL ones) */
	rc = parse_args(argc, argv);
	if (rc < 0)
		rte_exit(EXIT_FAILURE, "Invalid VIRTIO_L2FWD parameters\n");

	if (check_lcore_params() < 0)
		rte_exit(EXIT_FAILURE, "check_lcore_params() failed\n");

	rc = init_lcore_ethdev_rx();
	if (rc < 0)
		rte_exit(EXIT_FAILURE, "init_lcore_rx_queues() failed\n");

	rc = init_lcore_emdev_deq();
	if (rc < 0)
		rte_exit(EXIT_FAILURE, "init_lcore_virtio_dev() failed\n");

	if (check_port_config() < 0)
		APP_ERR("check_port_config() failed\n");

	/* Alloc mempools */
	setup_mempools();

	/* Initialize all ethdev ports. 8< */
	setup_eth_devices();

	/* Start ports */
	RTE_ETH_FOREACH_DEV(portid) {
		if (!is_ethdev_enabled(portid))
			continue;

		/* Start device */
		rc = rte_eth_dev_start(portid);
		if (rc < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start: err=%d, port=%d\n", rc, portid);

		if (promiscuous_on)
			rte_eth_promiscuous_enable(portid);
	}

	check_all_ports_link_status();

	if (per_port_pool) {
		RTE_ETH_FOREACH_DEV(portid) {
			if (!is_ethdev_enabled(portid))
				continue;

			APP_ERR("Initial Packet pool avail buff_cnt=%d\n",
				rte_mempool_avail_count(e_pktmbuf_pool[portid]));
		}
	} else {
		APP_ERR("Initial Packet pool avail buff_cnt=%d\n",
			rte_mempool_avail_count(e_pktmbuf_pool[0]));
	}

	/* Close eth devices */
	release_eth_devices();

	/* clean up the EAL */
	rte_eal_cleanup();
	APP_INFO("Bye...\n");

	return rc;
}
