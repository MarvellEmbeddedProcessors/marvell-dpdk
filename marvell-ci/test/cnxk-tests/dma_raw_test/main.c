/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2026 Marvell.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <dirent.h>

#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_dmadev.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_memory.h>

#define DPI_NB_DESCS 1024
#define DPI_BURST_REQ	256
#define DPI_MAX_VFS	32
#define MAX_POINTERS	15

#define MAX_VCHANS	16
#define NUM_VCHANS	8

#define CNXK_PCI_VENDOR_ID	0x177dU
#define CN10K_DPI_PF_DEV_ID	0xa080U
#define CN10K_DPI_VF_DEV_ID	0xa081U
#define CN20K_DPI_PF_DEV_ID	0xa0e8U
#define CN20K_DPI_VF_DEV_ID	0xa0e9U

enum dpi_platform {
	DPI_PLAT_UNKNOWN = 0,
	DPI_PLAT_CN10K,
	DPI_PLAT_CN20K,
};

static volatile bool force_quit;
static volatile int worker_error;
static enum dpi_platform dpi_plat = DPI_PLAT_UNKNOWN;

static void
worker_fail(int err)
{
	force_quit = true;
	if (worker_error == 0)
		worker_error = err;
}

struct dpi_test_dev_s {
	int dev_id;
	struct rte_dma_info dev_info;
	struct rte_dma_vchan_conf vchan_conf[MAX_VCHANS];
};

struct dpi_test_dev_s dpi_test[DPI_MAX_VFS];
rte_iova_t raddr = 0, laddr;
int mode, n_iter = 60, perf_mode = 0;
int ptrs_per_instr = 1;
int burst_size = 64;
uint16_t done_count = 8;
uint16_t nb_ports;
uint16_t nb_vchans;
uint16_t pem_id;
uint64_t data_size = 128;

static rte_atomic64_t dma_submit_cnt[DPI_MAX_VFS];
static uint64_t last_dma_submit_cnt[DPI_MAX_VFS];
uint64_t total_dma_cnt;
static uint64_t timer_period = 1; /* default period is 1 seconds */

static const char *
dpi_platform_str(enum dpi_platform plat)
{
	switch (plat) {
	case DPI_PLAT_CN10K:
		return "cn10k";
	case DPI_PLAT_CN20K:
		return "cn20k";
	default:
		return "unknown";
	}
}

static bool
compat_entry_has(const char *entry, size_t len, const char *sub)
{
	size_t sublen = strlen(sub);
	size_t i;

	if (len < sublen)
		return false;

	for (i = 0; i <= len - sublen; i++) {
		if (memcmp(entry + i, sub, sublen) == 0)
			return true;
	}

	return false;
}

static enum dpi_platform
detect_dpi_platform_from_dt(void)
{
	FILE *f;
	char buf[256];
	const char *p, *end;
	size_t n, len;
	enum dpi_platform plat = DPI_PLAT_UNKNOWN;

	f = fopen("/proc/device-tree/compatible", "rb");
	if (!f)
		return DPI_PLAT_UNKNOWN;

	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (n == 0)
		return DPI_PLAT_UNKNOWN;

	end = buf + n;
	for (p = buf; p < end; p += len + 1) {
		len = strnlen(p, end - p);
		if (len == 0)
			continue;

		if (compat_entry_has(p, len, "cn20k")) {
			plat = DPI_PLAT_CN20K;
			break;
		}
		if (compat_entry_has(p, len, "cn10k"))
			plat = DPI_PLAT_CN10K;
	}

	return plat;
}

static int
pci_sysfs_read_u16(const char *dev_dir, const char *name, uint16_t *val)
{
	char path[512];
	FILE *f;
	unsigned int v;

	snprintf(path, sizeof(path), "%s/%s", dev_dir, name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%x", &v) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*val = (uint16_t)v;
	return 0;
}

static enum dpi_platform
detect_dpi_platform_from_pci(void)
{
	DIR *dir;
	struct dirent *ent;
	enum dpi_platform plat = DPI_PLAT_UNKNOWN;
	char dev_dir[512];
	uint16_t vendor, device;

	dir = opendir("/sys/bus/pci/devices");
	if (!dir)
		return DPI_PLAT_UNKNOWN;

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		snprintf(dev_dir, sizeof(dev_dir), "/sys/bus/pci/devices/%s",
			 ent->d_name);
		if (pci_sysfs_read_u16(dev_dir, "vendor", &vendor) < 0)
			continue;
		if (vendor != CNXK_PCI_VENDOR_ID)
			continue;
		if (pci_sysfs_read_u16(dev_dir, "device", &device) < 0)
			continue;

		if (device == CN20K_DPI_PF_DEV_ID ||
		    device == CN20K_DPI_VF_DEV_ID) {
			plat = DPI_PLAT_CN20K;
			break;
		}
		if (device == CN10K_DPI_PF_DEV_ID ||
		    device == CN10K_DPI_VF_DEV_ID) {
			if (plat == DPI_PLAT_UNKNOWN)
				plat = DPI_PLAT_CN10K;
		}
	}
	closedir(dir);

	return plat;
}

static enum dpi_platform
detect_dpi_platform(void)
{
	enum dpi_platform plat;

	plat = detect_dpi_platform_from_dt();
	if (plat != DPI_PLAT_UNKNOWN)
		return plat;

	return detect_dpi_platform_from_pci();
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM || signum == SIGQUIT) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

static uint8_t buffer_fill(uint8_t *addr, int len, uint8_t val)
{
	int j = 0;

	memset(addr, 0, len);
	for (j = 0; j < len; j++)
		*(addr + j) = val++;

	return val;
}

static inline void dump_stats(void)
{
	int i;
	uint64_t tot = 0;
	uint64_t cur, delta;

	for (i = 0; i < nb_vchans; i++) {
		cur = rte_atomic64_read(&dma_submit_cnt[i]);
		delta = cur - last_dma_submit_cnt[i];
		printf("DMA %d Count %ld %s %2.2f Gbps\n", i,
		       delta,
		       (mode == 3 ? (i % 2 ? "Outbound":"Inbound"):""),
		       (delta * data_size * ptrs_per_instr * 8) / 1000000000.0);
		tot += cur;
		last_dma_submit_cnt[i] = cur;
	}
	printf("\ntot %ld tot_dma %ld Total: %ld Perf:%2.2f Gbps\n", tot,
	       total_dma_cnt, (tot - total_dma_cnt),
	       ((tot - total_dma_cnt) * data_size * ptrs_per_instr * 8)/1000000000.0);

	total_dma_cnt = tot;
}

static inline int dma_test_xfer_perf(void)
{
	struct rte_dma_sge src[DPI_BURST_REQ][MAX_POINTERS], dst[DPI_BURST_REQ][MAX_POINTERS];
	uint64_t prev_tsc = 0, diff_tsc = 0, cur_tsc = 0;
	int ret = 0, vchan = 0, i, j, worker_id = 0, counter = 0;
	uint8_t *fptr[DPI_BURST_REQ][MAX_POINTERS];
	uint8_t *lptr[DPI_BURST_REQ][MAX_POINTERS];
	uint8_t num_req, max_ptr = ptrs_per_instr;
	unsigned int dma_port = rte_lcore_id();
	struct rte_dma_vchan_conf *vconf;
	int xfer_mode, direction;
	uint8_t *remote_addr;
	rte_iova_t src_ptr;
	rte_iova_t dst_ptr;

	if (dma_port == rte_get_main_lcore()) {
		printf("DMA port::: %u  main_locore:: %u\n", dma_port, rte_get_main_lcore());

		do {
			cur_tsc = rte_rdtsc();
			diff_tsc = cur_tsc - prev_tsc;
			if ((timer_period > 0) && (diff_tsc > timer_period)) {
				dump_stats();
				prev_tsc = cur_tsc;
				counter++;
				if (n_iter > 0 && counter >= n_iter)
					force_quit = true;
			}

			if (unlikely(force_quit)) {
				printf("lcore %d quitting.\n", dma_port);
				sleep(1);
				return 0;
			}
		} while (1);
	}

	dma_port -= 1;
	remote_addr = (uint8_t *)raddr + (dma_port * 0x4000000ull);
	printf("DMA_PORT:::: %u\n", dma_port);

	if (dma_port >= nb_vchans) {
		printf("lcore:: %u returning from data loop\n", rte_lcore_id());
		return 0;
	}
	/* for dual mode use even ports for inbound DMA
	 * and odd ports for outbound DMA
	 */
	if (mode == 3)
		xfer_mode = (dma_port % 2) ? 2 : 1;
	else
		xfer_mode = mode;

	if (xfer_mode == 1)
		direction = RTE_DMA_DIR_DEV_TO_MEM;
	else if (xfer_mode == 2)
		direction = RTE_DMA_DIR_MEM_TO_DEV;
	else
		direction = RTE_DMA_DIR_MEM_TO_MEM;

	printf("dma_port %d, raddr %p mode %d, xmode %d dir %d\n", dma_port, remote_addr,
		mode, xfer_mode, direction);

	/* Alloc ptrs */
	for (i = 0; i < burst_size; i++) {
		for (j = 0; j < max_ptr; j++) {
			lptr[i][j] = remote_addr + (i * max_ptr * data_size);
			fptr[i][j] = (uint8_t *)rte_malloc("xfer_block", data_size, 128);
			if (!fptr[i][j]) {
				printf("Unable to allocate internal memory\n");
				worker_fail(-ENOMEM);
				return -ENOMEM;
			}
			buffer_fill(fptr[i][j], data_size, 0);
			src_ptr = rte_malloc_virt2iova(fptr[i][j]);

			/* alloc for internal-only DMA */
			if (!xfer_mode) {
				lptr[i][j] = (uint8_t *)rte_malloc("xfer_block", data_size, 128);
				if (!lptr[i][j]) {
					printf("Unable to allocate internal memory\n");
					worker_fail(-ENOMEM);
					return -ENOMEM;
				}
				buffer_fill(lptr[i][j], data_size, 0);
				dst_ptr = rte_malloc_virt2iova(lptr[i][j]);
			} else {
				lptr[i][j] = lptr[i][j] + (j * data_size);
				dst_ptr = src_ptr;
			}

			switch (direction) {
			/* outbound */
			case RTE_DMA_DIR_MEM_TO_DEV:
				src[i][j].addr = src_ptr;
				src[i][j].length = data_size;
				dst[i][j].addr = (rte_iova_t)lptr[i][j];
				dst[i][j].length = data_size;
				break;
			/* inbound */
			case RTE_DMA_DIR_DEV_TO_MEM:
				src[i][j].addr = (rte_iova_t)lptr[i][j];
				src[i][j].length = data_size;
				dst[i][j].addr = dst_ptr;
				dst[i][j].length = data_size;
				break;
			/* internal_only */
			case RTE_DMA_DIR_MEM_TO_MEM:
				src[i][j].addr = src_ptr;
				src[i][j].length = data_size;

				dst[i][j].addr = dst_ptr;
				dst[i][j].length = data_size;
				break;
			};
		}
	}

	worker_id = dma_port;

	if (dpi_plat == DPI_PLAT_CN20K) {
		vchan = dma_port;
		dma_port = 0;
	}

	vconf = &dpi_test[dma_port].vchan_conf[vchan];

	vconf->direction = direction;
	vconf->nb_desc = DPI_NB_DESCS;

	if (dpi_plat != DPI_PLAT_CN20K) {
		switch (direction) {
		/* outbound */
		case RTE_DMA_DIR_MEM_TO_DEV:
			vconf->dst_port.port_type = RTE_DMA_PORT_PCIE;
			vconf->dst_port.pcie.coreid = pem_id;
			break;
		/* inbound */
		case RTE_DMA_DIR_DEV_TO_MEM:
			vconf->src_port.port_type = RTE_DMA_PORT_PCIE;
			vconf->src_port.pcie.coreid = pem_id;
			break;
		/* internal_only */
		case RTE_DMA_DIR_MEM_TO_MEM:
			break;
		};

		vchan = rte_dma_vchan_setup(dma_port, vchan, vconf);
		if (vchan < 0) {
			ret = vchan;
			printf("DMA vchan setup failed with err %d\n", ret);
			goto free_buf;
		}

		rte_dma_start(dma_port);
	}

	for (i = 0; i < burst_size; i++) {
		ret = rte_dma_copy_sg(dma_port, vchan, src[i], dst[i], max_ptr,
					 max_ptr, RTE_DMA_OP_FLAG_SUBMIT);
		if (ret < 0) {
			printf("dmadev copy op failed, ret=%d\n", ret);
			goto free_buf;
		}
	}
	num_req = i;

	uint16_t nb_done = 0, last_idx = 0;
	bool dma_error = false;

	do {
		if (unlikely(force_quit)) {
			printf("worker_id %d quitting.\n", worker_id);
			sleep(3);
			goto free_buf;
		}

		nb_done = 0;
		do {
			nb_done += rte_dma_completed(dma_port, vchan, num_req, &last_idx,
						     &dma_error);
		} while (nb_done < done_count);

		if (unlikely(dma_error)) {
			printf("DMA error on worker %d\n", worker_id);
			ret = -EIO;
			goto free_buf;
		}

		rte_atomic64_add(&dma_submit_cnt[worker_id], nb_done);
		num_req = nb_done;

		for (i = 0; i < num_req; i++) {
			ret = rte_dma_copy_sg(dma_port, vchan, src[i], dst[i], max_ptr,
						 max_ptr, 0);
			if (ret < 0) {
				printf("dmadev copy_sg op failed, ret=%d\n", ret);
				goto free_buf;
			}
		}

		rte_dma_submit(dma_port, vchan);
	} while (1);

free_buf:
	if (ret < 0)
		worker_fail(ret);
	else
		ret = 0;

	printf("dma_port %d freeing memory.\n", dma_port);
	for (i = 0; i < burst_size; i++) {
		for (j = 0; j < max_ptr; j++) {
			if (fptr[i][j])
				rte_free(fptr[i][j]);
			if (!xfer_mode && lptr[i][j])
				rte_free(lptr[i][j]);
		}
	}

	return ret;
}

static int
launch_one_lcore(void *dummy)
{
	int rc;

	RTE_SET_USED(dummy);
	rc = dma_test_xfer_perf();
	if (rc < 0)
		worker_fail(rc);

	return rc;
}

static uint64_t dpi_parse_addr(const char *q_arg)
{
	char *end = NULL;
	uint64_t n;

	/* parse number string */
	n = strtoul(q_arg, &end, 0);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	return n;
}

/* display usage */
static void dpi_usage(const char *prgname)
{
	printf("%s [EAL options] --\n"
		"  -r <remote address>: Remote pointer\n"
		"  -l <first address>: This is also remote address valid for external only mode\n"
		"  -m <mode>: Mode of transfer\n"
		"             0: Internal Only\n"
		"             1: Inbound\n"
		"             2: Outbound\n"
		"             3: Dual - both inbound and outbound (supported only in perf test)\n"
		"             4: Inbound Latency\n"
		"             5: Outbound Latency\n"
		"             6: Inbound roundtrip Latency\n"
		"             7: Outbound roundtrip Latency\n"
		"             8: All modes 4, 5, 6, 7\n"
		"		Max data size for latency tests :\n"
		"			(65535*15) = 983025 bytes\n"
		"  -i <iteration>: No.of iterations\n"
		"  -s <data size>: Size of data to be DMA'ed (Default is 256)\n"
		"  -z <pem number>: PEM connected to host\n"
		"  -b <burst size>: Initial number of packets submitted to the DMA\n"
		"  -d <done count>: Min numbers of completions to wait for before resubmission\n"
		"  -p: Performance test\n"
		"  -t <num>: Number of pointers per instruction (Default is 1)\n"
		"  --inb_sz <data size>: Size of Inbound data size in mode 3\n"
		"  --outb_sz <data size>: Size of Outbound data size in mode 3\n", prgname);
}

/* Parse the argument given in the command line of the application */
static int dpi_parse_args(int argc, char **argv)
{
	int opt, ret, opt_idx;
	char **argvopt;
	char *prgname = argv[0];

	static struct option lgopts[] = {
		{ "inb_sz", 1, 0, 0},
		{ "outb_sz", 1, 0, 0},
		{0, 0, 0, 0 },
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "r:s:l:m:i:pb:z:t:d:", lgopts, &opt_idx)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'r':
			raddr = dpi_parse_addr(optarg);
			if ((long)raddr == 0) {
				printf("invalid remote address\n");
				dpi_usage(prgname);
				return -1;
			}
			printf("raddr: 0x%lx\n", raddr);
			break;
		case 's':
			data_size = dpi_parse_addr(optarg);
			if ((long)data_size == 0) {
				printf("invalid data Size\n");
				dpi_usage(prgname);
				return -1;
			}
			printf("data_size: 0x%lx\n", data_size);
			break;
		case 'l':
			laddr = dpi_parse_addr(optarg);
			if ((long)laddr < 0) {
				printf("invalid local address\n");
				dpi_usage(prgname);
				return -1;
			}
			break;
		case 'm':
			mode = atoi(optarg);
			printf("Mode: %d\n", mode);
			break;
		case 'i':
			n_iter = atoi(optarg);
			break;
		case 'z':
			pem_id = atoi(optarg);
			if (pem_id)
				pem_id = 1;
			break;
		case 'b':
			burst_size = atoi(optarg);
			printf("Burst size:: %d\n", burst_size);
			break;
		case 'd':
			done_count = atoi(optarg);
			break;
		case 'p':
			perf_mode = 1;
			break;
		case 't':
			ptrs_per_instr = atoi(optarg);
			printf("Pointers per instr: %d\n", ptrs_per_instr);
			break;
		case 0: /* long options */
			if (!strcmp(lgopts[opt_idx].name, "inb_sz")) {
				printf("This option is not yet supported\n");
				return -1;
			}
			if (!strcmp(lgopts[opt_idx].name, "outb_sz")) {
				printf("This option is not yet supported\n");
				return -1;
			}
			break;
		default:
			dpi_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	optind = 1; /* reset getopt lib */
	return 0;
}

int main(int argc, char **argv)
{
	struct rte_dma_conf dev_conf = { 0 };
	struct rte_dma_vchan_conf *vconf;
	unsigned int lcore_id;
	int ret, i, j;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	worker_error = 0;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGQUIT, signal_handler);
	signal(SIGUSR1, signal_handler);

	total_dma_cnt = 0;
	pem_id = 0;
	/* parse application arguments (after the EAL ones) */
	ret = dpi_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid App arguments\n");

	dpi_plat = detect_dpi_platform();
	printf("DPI platform: %s\n", dpi_platform_str(dpi_plat));
	if (dpi_plat == DPI_PLAT_UNKNOWN)
		printf("Warning: could not determine CN10K/CN20K from DT or PCI IDs\n");

	nb_ports = rte_dma_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No dmadev ports - bye\n");

	if (dpi_plat == DPI_PLAT_CN20K)
		nb_ports = 1;

	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();

	printf("%d dmadev ports detected\n", nb_ports);

	if (burst_size < done_count || burst_size > 256)
		rte_exit(EXIT_FAILURE, "Burst_size must be between %u to 256\n", done_count);

	/* Configure dmadev ports */
	for (i = 0; i < nb_ports; i++) {
		dpi_test[i].dev_id = i;
		rte_dma_info_get(i, &dpi_test[i].dev_info);
		if (ptrs_per_instr > dpi_test[i].dev_info.max_sges) {
			printf("Max pointers can be only %d\n",
			       dpi_test[i].dev_info.max_sges);
			goto err_exit;
		}
		dev_conf.nb_vchans = 1;
		nb_vchans = nb_ports;

		if (dpi_plat == DPI_PLAT_CN20K) {
			dev_conf.nb_vchans = NUM_VCHANS;
			nb_vchans = RTE_MIN(NUM_VCHANS, dpi_test[i].dev_info.max_vchans);
		}

		ret = rte_dma_configure(i, &dev_conf);
		if (ret)
			rte_exit(EXIT_FAILURE, "Unable to configure DPIVF %d\n",
				 i);
		printf("dmadev %d configured successfully\n", i);
	}

	if (dpi_plat == DPI_PLAT_CN20K) {
		i = 0;
		for (j = 0; j < nb_vchans; j++) {
			vconf = &dpi_test[i].vchan_conf[j];
			if (mode == 1)
				vconf->direction = RTE_DMA_DIR_DEV_TO_MEM;
			else if (mode == 2)
				vconf->direction = RTE_DMA_DIR_MEM_TO_DEV;
			else if (mode == 3)
				vconf->direction = (j % 2) ? RTE_DMA_DIR_MEM_TO_DEV :
						   RTE_DMA_DIR_DEV_TO_MEM;
			else
				vconf->direction = RTE_DMA_DIR_MEM_TO_MEM;
			vconf->nb_desc = DPI_NB_DESCS;

			switch (vconf->direction) {
				/* outbound */
			case RTE_DMA_DIR_MEM_TO_DEV:
				vconf->dst_port.port_type = RTE_DMA_PORT_PCIE;
				vconf->dst_port.pcie.coreid = pem_id;
				break;
				/* inbound */
			case RTE_DMA_DIR_DEV_TO_MEM:
				vconf->src_port.port_type = RTE_DMA_PORT_PCIE;
				vconf->src_port.pcie.coreid = pem_id;
				break;
				/* internal_only */
			case RTE_DMA_DIR_MEM_TO_MEM:
				break;
			case RTE_DMA_DIR_DEV_TO_DEV:
				/* Not used by this test; would need src/dst PCIe ports */
				break;
			};

			ret = rte_dma_vchan_setup(i, j, vconf);
			if (ret < 0) {
				printf("DMA vchan::%d setup failed with err %d\n", j, ret);
				goto err_exit;
			}
		}
		ret = rte_dma_start(i);
		if (ret < 0) {
			printf("DMA start failed for dmadev::%d, err %d\n", i, ret);
			goto err_exit;
		}
	}

	/* launch per-lcore init on every lcore */
	if (rte_eal_mp_remote_launch(launch_one_lcore, NULL, CALL_MAIN) < 0) {
		ret = -1;
		goto err_exit;
	}

	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		int lret = rte_eal_wait_lcore(lcore_id);

		if (lret != 0 && ret == 0)
			ret = lret;
	}

	if (worker_error != 0 && ret == 0)
		ret = worker_error;

err_exit:
	for (i = 0; i < nb_ports; i++) {
		rte_dma_stop(i);
		if (rte_dma_close(i))
			printf("Dev close failed for port %d\n", i);
	}

	rte_eal_cleanup();
	return ret;
}
