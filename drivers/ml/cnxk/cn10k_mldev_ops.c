/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2022 Marvell.
 */

#include <eal_firmware.h>
#include <mldev_pmd.h>
#include <rte_eal.h>
#include <rte_mldev.h>

#include "cn10k_mldev.h"
#include "cn10k_mldev_ops.h"
#include "cnxk_mldev.h"

#include "roc_api.h"

/* ML firmware macros */
#define FW_MEMZONE_NAME	 "ml_cn10k_fw_mz"
#define FW_BUFFER_SIZE	 0x800000
#define FW_LINKER_OFFSET 0x80000
#define FW_WAIT_CYCLES	 100

/* ML configuration macros */
#define ML_CONFIG_MEMZONE_NAME "ml_cn10k_config_mz"
#define ML_JOBPOOL_NAME	       "ml_cn10k_model_jobpool"
#define ML_JOBPOOL_SIZE	       1024

/* ML model macros */
#define ML_MODEL_MEMZONE_NAME "ml_cn10k_model_mz"
#define ML_MODEL_JD_POOL_SIZE 0x20
#define JD_LOAD		      (ML_MODEL_JD_POOL_SIZE - 2)
#define JD_UNLOAD	      (ML_MODEL_JD_POOL_SIZE - 1)

/* ML Job descriptor flags */
#define ML_FLAGS_POLL_COMPL BIT(0)
#define ML_FLAGS_SSO_COMPL  BIT(1)
#define ML_FLAGS_CMPC_COMPL BIT(2)

/* Timeout */
#define ML_TIMEOUT_FW_LOAD_S  10
#define ML_TIMEOUT_LOAD_S     20
#define ML_TIMEOUT_UNLOAD_S   5
#define ML_TIMEOUT_RUN_S      10
#define ML_TIMEOUT_MULTIPLIER 150

/* Job status */
#define ML_STATUS_SUCCESS 0x0
#define ML_STATUS_FAILURE 0xFF

#define BYTE_LEN	  8
#define OCM_MAP_WORD_SIZE (sizeof(uint8_t) * BYTE_LEN)

#define ML_MOD_INC(i, l) ((i) == (l - 1) ? (i) = 0 : (i)++)

static void
cn10k_ml_fw_print_info(struct cnxk_ml_fw *ml_fw)
{
	plt_ml_dbg("Firmware capabilities = 0x%016lx", ml_fw->load_fw->cap.u);
	plt_ml_dbg("Version = %s", ml_fw->load_fw->version);
	plt_ml_dbg("fw_core0_dbg_ptr = 0x%016lx",
		   ml_fw->load_fw->debug_cap.fw_core0_dbg_ptr);
	plt_ml_dbg("fw_core1_dbg_ptr = 0x%016lx",
		   ml_fw->load_fw->debug_cap.fw_core1_dbg_ptr);
	plt_ml_dbg("fw_dbg_buffer_size = 0x%08x",
		   ml_fw->load_fw->debug_cap.fw_dbg_buffer_size);
	plt_ml_dbg("fw_core0_exception_state = 0x%016lx",
		   ml_fw->load_fw->debug_cap.fw_core0_exception_buffer);
	plt_ml_dbg("fw_core1_exception_state = 0x%016lx",
		   ml_fw->load_fw->debug_cap.fw_core1_exception_buffer);
	plt_ml_dbg("fw_exception_state_size = 0x%08x",
		   ml_fw->load_fw->debug_cap.fw_exception_state_size);
}

static int
cn10k_ml_fw_load_asim(struct cnxk_ml_fw *ml_fw)
{
	struct cnxk_ml_dev *ml_dev;
	uint64_t wait_cycles;
	uint64_t start_cycle;
	uint64_t reg_val64;
	bool timeout;
	int ret = 0;

	ml_dev = ml_fw->ml_dev;

	/* Set ML_MLR_BASE to base IOVA of the ML region in LLC/DRAM. */
	reg_val64 = rte_eal_get_baseaddr();
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_MLR_BASE);
	plt_ml_dbg("ML_MLR_BASE = 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_MLR_BASE));

	/* Reset scratch registers */
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_AP_FW_COMM);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_WORK_PTR);

	/* Update FW load completion structure */
	ml_fw->load_fw->hdr.compl_W1.status_ptr =
		PLT_U64_CAST(&ml_fw->status_ptr);
	ml_fw->load_fw->hdr.command = CNXK_ML_JOB_CMD_FW_LOAD;
	ml_fw->load_fw->hdr.job_result =
		roc_ml_addr_ap2mlip(&ml_dev->roc, &ml_fw->job_result);
	plt_write64(ML_CN10K_POLL_JOB_START, &ml_fw->status_ptr);
	plt_wmb();

	/* Enqueue FW load through scratch registers */
	roc_ml_clk_force_on(&ml_dev->roc);
	roc_ml_dma_stall_off(&ml_dev->roc);
	roc_ml_scratch_enqueue(&ml_dev->roc, ml_fw->load_fw);

	/* Wait for notification from firmware that ML is ready for job
	 * execution.
	 */
	timeout = true;
	wait_cycles =
		ML_TIMEOUT_FW_LOAD_S * ML_TIMEOUT_MULTIPLIER * plt_tsc_hz();
	start_cycle = plt_tsc_cycles();
	plt_rmb();
	do {
		if (roc_ml_scratch_is_done_bit_set(&ml_dev->roc) &&
		    (plt_read64(&ml_fw->status_ptr) ==
		     ML_CN10K_POLL_JOB_FINISH)) {
			timeout = false;
			break;
		}
	} while (plt_tsc_cycles() - start_cycle < wait_cycles);

	roc_ml_dma_stall_on(&ml_dev->roc);
	roc_ml_clk_force_off(&ml_dev->roc);

	if ((!timeout) && (ml_fw->job_result.status == ML_STATUS_SUCCESS)) {
		cn10k_ml_fw_print_info(ml_fw);
	} else {
		/* Set ML to disable new jobs */
		reg_val64 = (ROC_ML_CFG_JD_SIZE | ROC_ML_CFG_MLIP_ENA);
		roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);

		/* Clear scratch registers */
		roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_WORK_PTR);
		roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_AP_FW_COMM);

		if (timeout) {
			plt_err("Firmware load timeout");
			ret = -ETIME;
		} else {
			plt_err("Firmware load failed");
			ret = -1;
		}

		return ret;
	}

	/* Reset scratch registers */
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_AP_FW_COMM);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_WORK_PTR);

	/* Disable job execution, to be enabled in start */
	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_CFG);
	reg_val64 &= ~ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_CFG));

	return ret;
}

static int
cn10k_ml_fw_load_cn10ka(struct cnxk_ml_fw *ml_fw, void *buffer, uint64_t size)
{
	struct cnxk_ml_dev *ml_dev;
	uint64_t wait_cycles;
	uint64_t start_cycle;
	uint64_t reg_val64;
	uint32_t reg_val32;
	uint64_t offset;
	bool timeout;
	uint32_t w0;
	uint32_t w1;
	int ret = 0;
	uint8_t i;

	ml_dev = ml_fw->ml_dev;

	/* Reset HEAD and TAIL debug pointer registers */
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_DBG_BUFFER_HEAD_C0);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_DBG_BUFFER_TAIL_C0);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_DBG_BUFFER_HEAD_C1);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_DBG_BUFFER_TAIL_C1);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_EXCEPTION_SP_C0);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_EXCEPTION_SP_C1);

	/* (1) Write firmware images for ACC's two A35 cores to the ML region in
	 * LLC / DRAM.
	 */
	memcpy(PLT_PTR_ADD(ml_fw->data_fw, FW_LINKER_OFFSET), buffer, size);

	/* (2) Set ML(0)_MLR_BASE = Base IOVA of the ML region in LLC/DRAM.
	 */
	reg_val64 =
		PLT_PTR_SUB_U64_CAST(ml_fw->data_fw, rte_eal_get_baseaddr());
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_MLR_BASE);
	plt_ml_dbg("ML_MLR_BASE => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_MLR_BASE));

	/* (3) Set ML(0)_AXI_BRIDGE_CTRL(1) = 0x184003 to remove backpressure
	 * check on DMA AXI bridge.
	 */
	reg_val64 = (ROC_ML_AXI_BRIDGE_CTRL_AXI_RESP_CTRL |
		     ROC_ML_AXI_BRIDGE_CTRL_BRIDGE_CTRL_MODE |
		     ROC_ML_AXI_BRIDGE_CTRL_NCB_WR_BLK |
		     ROC_ML_AXI_BRIDGE_CTRL_FORCE_WRESP_OK |
		     ROC_ML_AXI_BRIDGE_CTRL_FORCE_RRESP_OK);
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_AXI_BRIDGE_CTRL(1));
	plt_ml_dbg("ML_AXI_BRIDGE_CTRL(1) => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_AXI_BRIDGE_CTRL(1)));

	/* (4) Set ML(0)_ANB(0..2)_BACKP_DISABLE = 0x3 to remove backpressure on
	 * the AXI to NCB bridges.
	 */
	for (i = 0; i < ML_ANBX_NR; i++) {
		reg_val64 = (ROC_ML_ANBX_BACKP_DISABLE_EXTMSTR_B_BACKP_DISABLE |
			     ROC_ML_ANBX_BACKP_DISABLE_EXTMSTR_R_BACKP_DISABLE);
		roc_ml_reg_write64(&ml_dev->roc, reg_val64,
				   ML_ANBX_BACKP_DISABLE(i));
		plt_ml_dbg("ML_ANBX_BACKP_DISABLE(%d) => 0x%016lx", i,
			   roc_ml_reg_read64(&ml_dev->roc,
					     ML_ANBX_BACKP_DISABLE(i)));
	}

	/* (5) Set ML(0)_ANB(0..2)_NCBI_P_OVR = 0x3000 and
	 * ML(0)_ANB(0..2)_NCBI_NP_OVR = 0x3000 to signal all ML transactions as
	 * non-secure.
	 */
	for (i = 0; i < ML_ANBX_NR; i++) {
		reg_val64 = (ML_ANBX_NCBI_P_OVR_ANB_NCBI_P_NS_OVR |
			     ML_ANBX_NCBI_P_OVR_ANB_NCBI_P_NS_OVR_VLD);
		roc_ml_reg_write64(&ml_dev->roc, reg_val64,
				   ML_ANBX_NCBI_P_OVR(i));
		plt_ml_dbg(
			"ML_ANBX_NCBI_P_OVR(%d) => 0x%016lx", i,
			roc_ml_reg_read64(&ml_dev->roc, ML_ANBX_NCBI_P_OVR(i)));

		reg_val64 |= (ML_ANBX_NCBI_NP_OVR_ANB_NCBI_NP_NS_OVR |
			      ML_ANBX_NCBI_NP_OVR_ANB_NCBI_NP_NS_OVR_VLD);
		roc_ml_reg_write64(&ml_dev->roc, reg_val64,
				   ML_ANBX_NCBI_NP_OVR(i));
		plt_ml_dbg("ML_ANBX_NCBI_NP_OVR(%d) => 0x%016lx", i,
			   roc_ml_reg_read64(&ml_dev->roc,
					     ML_ANBX_NCBI_NP_OVR(i)));
	}

	/* (6) Set ML(0)_CFG[MLIP_CLK_FORCE] = 1, to force turning on the MLIP
	 * clock.
	 */
	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_CFG);
	reg_val64 |= ROC_ML_CFG_MLIP_CLK_FORCE;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_CFG));

	/* (7) Set ML(0)_JOB_MGR_CTRL[STALL_ON_IDLE] = 0, to make sure the boot
	 * request is accepted when there is no job in the command queue.
	 */
	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_JOB_MGR_CTRL);
	reg_val64 &= ~ROC_ML_JOB_MGR_CTRL_STALL_ON_IDLE;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_JOB_MGR_CTRL);
	plt_ml_dbg("ML_JOB_MGR_CTRL => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_JOB_MGR_CTRL));

	/* (8) Set ML(0)_CFG[ENA] = 0 and ML(0)_CFG[MLIP_ENA] = 1 to bring MLIP
	 * out of reset while keeping the job manager disabled.
	 */
	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_CFG);
	reg_val64 |= ROC_ML_CFG_MLIP_ENA;
	reg_val64 &= ~ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_CFG));

	/* (9) Wait at least 70 coprocessor clock cycles.
	 */
	plt_delay_us(FW_WAIT_CYCLES);

	/* Update FW load completion structure */
	ml_fw->load_fw->hdr.compl_W1.status_ptr =
		PLT_U64_CAST(&ml_fw->status_ptr);
	ml_fw->load_fw->hdr.command = CNXK_ML_JOB_CMD_FW_LOAD;
	ml_fw->load_fw->hdr.job_result =
		roc_ml_addr_ap2mlip(&ml_dev->roc, &ml_fw->job_result);
	plt_write64(ML_CN10K_POLL_JOB_START, &ml_fw->status_ptr);
	plt_wmb();

	/* Enqueue FW load through scratch registers */
	roc_ml_scratch_enqueue(&ml_dev->roc, ml_fw->load_fw);

	/* (10) Write ML outbound addresses pointing to the firmware images
	 * written in step 1 to the following registers:
	 * ML(0)_A35_0_RST_VECTOR_BASE_W(0..1) for core 0,
	 * ML(0)_A35_1_RST_VECTOR_BASE_W(0..1) for core 1. The value written to
	 * each register is the AXI outbound address divided by 4. Read after
	 * write.
	 */
	offset = PLT_PTR_ADD_U64_CAST(
		ml_fw->data_fw,
		FW_LINKER_OFFSET -
			roc_ml_reg_read64(&ml_dev->roc, ML_MLR_BASE));
	offset = (offset + ML_AXI_START_ADDR) / 4;
	w0 = PLT_U32_CAST(offset & 0xFFFFFFFFLL);
	w1 = PLT_U32_CAST(offset >> 32);

	roc_ml_reg_write32(&ml_dev->roc, w0, ML_A35_0_RST_VECTOR_BASE_W(0));
	reg_val32 =
		roc_ml_reg_read32(&ml_dev->roc, ML_A35_0_RST_VECTOR_BASE_W(0));
	plt_ml_dbg("ML_A35_0_RST_VECTOR_BASE_W(0) => 0x%08x", reg_val32);

	roc_ml_reg_write32(&ml_dev->roc, w1, ML_A35_0_RST_VECTOR_BASE_W(1));
	reg_val32 =
		roc_ml_reg_read32(&ml_dev->roc, ML_A35_0_RST_VECTOR_BASE_W(1));
	plt_ml_dbg("ML_A35_0_RST_VECTOR_BASE_W(1) => 0x%08x", reg_val32);

	roc_ml_reg_write32(&ml_dev->roc, w0, ML_A35_1_RST_VECTOR_BASE_W(0));
	reg_val32 =
		roc_ml_reg_read32(&ml_dev->roc, ML_A35_1_RST_VECTOR_BASE_W(0));
	plt_ml_dbg("ML_A35_1_RST_VECTOR_BASE_W(0) => 0x%08x", reg_val32);

	roc_ml_reg_write32(&ml_dev->roc, w1, ML_A35_1_RST_VECTOR_BASE_W(1));
	reg_val32 =
		roc_ml_reg_read32(&ml_dev->roc, ML_A35_1_RST_VECTOR_BASE_W(1));
	plt_ml_dbg("ML_A35_1_RST_VECTOR_BASE_W(1) => 0x%08x", reg_val32);

	/* (11) Clear MLIP’s ML(0)_SW_RST_CTRL[ACC_RST]. This will bring the ACC
	 * cores and other MLIP components out of reset. The cores will execute
	 * firmware from the ML region as written in step 1.
	 */
	reg_val32 = roc_ml_reg_read32(&ml_dev->roc, ML_SW_RST_CTRL);
	reg_val32 &= ~ROC_ML_SW_RST_CTRL_ACC_RST;
	roc_ml_reg_write32(&ml_dev->roc, reg_val32, ML_SW_RST_CTRL);
	reg_val32 = roc_ml_reg_read32(&ml_dev->roc, ML_SW_RST_CTRL);
	plt_ml_dbg("ML_SW_RST_CTRL => 0x%08x", reg_val32);

	/* (12) Wait for notification from firmware that ML is ready for job
	 * execution.
	 */
	timeout = true;
	wait_cycles = ML_TIMEOUT_FW_LOAD_S * plt_tsc_hz();
	start_cycle = plt_tsc_cycles();
	plt_rmb();
	do {
		if (roc_ml_scratch_is_done_bit_set(&ml_dev->roc) &&
		    (plt_read64(&ml_fw->status_ptr) ==
		     ML_CN10K_POLL_JOB_FINISH)) {
			timeout = false;
			break;
		}
	} while (plt_tsc_cycles() - start_cycle < wait_cycles);

	/* Check firmware load status, clean-up and exit on failure. */
	if ((!timeout) && (ml_fw->job_result.status == ML_STATUS_SUCCESS)) {
		cn10k_ml_fw_print_info(ml_fw);
	} else {
		/* Set ML to disable new jobs */
		reg_val64 = (ROC_ML_CFG_JD_SIZE | ROC_ML_CFG_MLIP_ENA);
		roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);

		/* Clear scratch registers */
		roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_WORK_PTR);
		roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_AP_FW_COMM);

		if (timeout) {
			plt_err("Firmware load timeout");
			ret = -ETIME;
		} else {
			plt_err("Firmware load failed");
			ret = -1;
		}

		return ret;
	}

	/* (13) Set ML(0)_JOB_MGR_CTRL[STALL_ON_IDLE] = 0x1; this is needed to
	 * shut down the MLIP clock when there are no more jobs to process.
	 */
	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_JOB_MGR_CTRL);
	reg_val64 |= ROC_ML_JOB_MGR_CTRL_STALL_ON_IDLE;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_JOB_MGR_CTRL);
	plt_ml_dbg("ML_JOB_MGR_CTRL => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_JOB_MGR_CTRL));

	/* (14) Set ML(0)_CFG[MLIP_CLK_FORCE] = 0; the MLIP clock will be turned
	 * on/off based on job activities.
	 */
	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_CFG);
	reg_val64 &= ~ROC_ML_CFG_MLIP_CLK_FORCE;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_CFG));

	/* (15) Set ML(0)_CFG[ENA] to enable ML job execution. */
	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_CFG);
	reg_val64 |= ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_CFG));

	/* Reset scratch registers */
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_AP_FW_COMM);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_WORK_PTR);

	/* Disable job execution, to be enabled in start */
	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_CFG);
	reg_val64 &= ~ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_CFG));

	return ret;
}

/* Left shift multi-word mask by 1 bit.
 *
 * For example, given a mask of two uint8_t words
 * Input:  [00110101] [00110111]
 * Output: [01101010] [01101110]
 */
static void
lshift_mask(uint8_t *mask, int nwords)
{
	int i;
	int word_sz;

	word_sz = sizeof(uint8_t) * BYTE_LEN;
	for (i = nwords - 1; i >= 0; i--) {
		mask[i] = mask[i] << 1;
		if (i != 0)
			mask[i] = mask[i] | (mask[i - 1] >> (word_sz - 1));
	}
}

/* Get the index of the first unused slot in a multi-word mask. An unused slot
 * is a sequence of slot_sz continuous unset bits in the mult-word mask. For
 * example given a multi-word mask.
 *
 * The program creates a search_mask with slot_sz bits set. Uses a sliding
 * windows approach to scan the mask to identify the available first slot.
 * search_mask slides left from start_pos to end.
 *
 * [10111000] [01001001]
 * - WORD 1 --- WORD 0 -
 *
 * When start = 0,
 * Index of the first unused slot of size 4 is 7.
 * Index of the first unused slot of size 3 is 7.
 * Index of the first unused slot of size 2 is 1.
 * Index of the first unused slot of size 1 is 1.
 *
 * When start = 2,
 * Index of the first unused slot of size 4 is 7.
 * Index of the first unused slot of size 2 is 4.
 * Index of the first unused slot of size 1 is 2.
 */

static int
slot_index(uint8_t *base_mask, int nwords, int slot_sz, int start_pos)
{
	uint8_t *search_mask;
	uint8_t res;

	int word_sz;
	int end_pos;
	int i, j;
	int idx;

	idx = -1;
	word_sz = sizeof(uint8_t) * BYTE_LEN;

	/* Create a mask with slot_sz bits set */
	search_mask = plt_zmalloc(nwords * sizeof(uint8_t), 0);
	if (search_mask == NULL)
		goto error;

	for (i = 0; i < nwords; i++) {
		if (i < slot_sz / word_sz)
			search_mask[i] = 0xFF;
		else if (i > slot_sz / word_sz)
			search_mask[i] = 0x00;
		else
			search_mask[i] = (1 << (slot_sz % word_sz)) - 1;
	}

	/* Shift search mask by start_pos bits */
	for (i = 0; i < start_pos; i++)
		lshift_mask(search_mask, nwords);

	/* Scan for a slot, left shift search mask after every iteration */
	end_pos = nwords * word_sz - slot_sz + 1;
	for (j = start_pos; j < end_pos; j++) {
		res = 0x0;
		for (i = 0; i < nwords; i++)
			res = res | (base_mask[i] & search_mask[i]);

		if (res == 0) {
			idx = j;
			goto found_idx;
		}

		lshift_mask(search_mask, nwords);
	}

found_idx:
	plt_free(search_mask);

error:
	return idx;
}

/* Count number of bits in a tilemask. Assumes that all set bits are contiguous.
 */
static int
cnxk_ml_ocm_tilecount(uint64_t tilemask, int *start, int *end)
{
	uint8_t count;

	PLT_ASSERT(tilemask != 0);

	*start = __builtin_ctzl(tilemask);
	*end = 64 - __builtin_clzl(tilemask) - 1;
	count = *end - *start + 1;

	PLT_ASSERT(count == __builtin_popcountl(tilemask));
	return count;
}

/* Find a tilemask to load the model on 'num_tiles' tiles with given scratch and
 * wb pages. Optionally specify the start_tile to search for tilemask.
 *
 * When start_tile = -1, search is done with all possible values for start_tile.
 * When start_tile >= 0, search is restricted to the tiles (start_tile) to
 * (start_tile + num_tiles -1).
 *
 * Examples
 * 1. With num_tiles = 2 and start_tile = 2, tilemask that would be checked is
 * 0xC (1100b)
 * 2. With num_tiles = 2 and start_tile = 0, tilemask that would be checked is
 * 0x3 (0011b)
 * 3. With num_tiles = 2 and start_tile = -1, tilemask's that would be checked
 * are 0x3 (0011b), 0xC (1100b), 0x30 (11_0000b) and 0xC0 (1100_0000b). The
 * first suitable tilemask would be used.
 */
static int
cnxk_ml_ocm_tilemask_find(struct rte_mldev *dev, uint8_t num_tiles,
			  uint16_t wb_pages, uint16_t scratch_pages,
			  int start_tile, uint64_t *tilemask)
{
	uint8_t local_ocm_mask[ML_CN10K_OCM_MASKWORDS] = {0};
	struct cn10k_ml_ocm_tile_info *ocm_tile_info;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_dev *ml_dev;

	uint16_t used_scratch_pages_max;
	uint16_t scratch_page_start;
	int used_last_wb_page_max;
	uint16_t scratch_page_end;
	uint8_t search_start_tile;
	uint8_t search_end_tile;
	uint8_t tile_start;
	uint16_t tile_id;
	uint16_t word_id;
	int wb_page_start;
	int page_id;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ocm_tile_info =
		(struct cn10k_ml_ocm_tile_info *)(ml_config->ocm_tile_info);

	if (num_tiles > ML_CN10K_OCM_NUMTILES) {
		plt_err("Invalid num_tiles = %d (> %d)", num_tiles,
			ML_CN10K_OCM_NUMTILES);
		return -1;
	}

	if ((start_tile != -1) && (start_tile % num_tiles != 0)) {
		plt_err("Invalid start_tile, %d", start_tile);
		return -1;
	}

	memset(tilemask, 0, sizeof(uint64_t));
	wb_page_start = -1;
	used_scratch_pages_max = 0;
	used_last_wb_page_max = -1;

	if (start_tile < 0) {
		search_start_tile = 0;
		search_end_tile = ml_config->ocm_num_tiles - num_tiles;
	} else {
		search_start_tile = start_tile;
		search_end_tile = start_tile;
	}

	for (tile_start = search_start_tile; tile_start <= search_end_tile;
	     tile_start = tile_start + num_tiles) {
		for (tile_id = tile_start; tile_id < tile_start + num_tiles;
		     tile_id++) {
			used_scratch_pages_max =
				PLT_MAX(ocm_tile_info[tile_id].scratch_pages,
					used_scratch_pages_max);
			used_last_wb_page_max =
				PLT_MAX(ocm_tile_info[tile_id].last_wb_page,
					used_last_wb_page_max);
		}

		memset(local_ocm_mask, 0, sizeof(local_ocm_mask));
		for (tile_id = tile_start; tile_id < tile_start + num_tiles;
		     tile_id++) {
			for (word_id = 0; word_id < ml_config->ocm_mask_words;
			     word_id++)
				local_ocm_mask[word_id] |=
					ocm_tile_info[tile_id]
						.ocm_mask[word_id];
		}

		if (used_scratch_pages_max <
		    scratch_pages) { /* Check for extra scratch pages */
			if (ml_config->ocm_pages - used_last_wb_page_max - 1 >
			    scratch_pages) { /* Pages available */
				scratch_page_start =
					ml_config->ocm_pages - scratch_pages;
				scratch_page_end = ml_config->ocm_pages - 1;
				for (page_id = scratch_page_start;
				     page_id <= scratch_page_end;
				     page_id++) { /* Mark the extra scratch
						   * pages as used
						   */
					local_ocm_mask[page_id /
						       OCM_MAP_WORD_SIZE] =
						SET_BIT(local_ocm_mask
								[page_id /
								 OCM_MAP_WORD_SIZE],
							page_id %
								OCM_MAP_WORD_SIZE);
				}
			} else { /* Pages not available, check for next tileset
				  */
				continue;
			}
		}

		wb_page_start = slot_index(
			local_ocm_mask, ml_config->ocm_mask_words, wb_pages, 0);
		if (wb_page_start !=
		    -1) { /* Have a valid slot for WB, else next tileset */
			*tilemask = GENMASK_ULL(tile_start + num_tiles - 1,
						tile_start);
			return wb_page_start;
		}
	}

	return wb_page_start;
}

static void
cnxk_ml_ocm_reserve_pages(struct rte_mldev *dev, uint32_t model_id,
			  uint64_t tilemask, int wb_page_start,
			  uint16_t wb_pages, uint16_t scratch_pages)
{
	struct cn10k_ml_ocm_tile_info *ocm_tile_info;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_model *ml_model;
	struct cnxk_ml_dev *ml_dev;

	int scratch_page_start;
	int scratch_page_end;
	int wb_page_end;
	int tile_start;
	int tile_end;
	int tile_id;
	int page_id;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ml_model = ml_config->ml_models[model_id];
	ocm_tile_info =
		(struct cn10k_ml_ocm_tile_info *)(ml_config->ocm_tile_info);

	/* Get first set bit, tile_start */
	tile_start = 0;
	tile_end = 0;
	cnxk_ml_ocm_tilecount(tilemask, &tile_start, &tile_end);
	wb_page_end = wb_page_start + wb_pages - 1;
	scratch_page_start = ml_config->ocm_pages - scratch_pages;
	scratch_page_end = ml_config->ocm_pages - 1;

	/* Update ocm_tile_info */
	for (tile_id = tile_start; tile_id <= tile_end; tile_id++) {
		/* Scratch pages */
		for (page_id = scratch_page_start; page_id <= scratch_page_end;
		     page_id++)
			ocm_tile_info[tile_id]
				.ocm_mask[page_id / OCM_MAP_WORD_SIZE] =
				SET_BIT(ocm_tile_info[tile_id]
						.ocm_mask[page_id /
							  OCM_MAP_WORD_SIZE],
					page_id % OCM_MAP_WORD_SIZE);
		ocm_tile_info[tile_id].scratch_pages = PLT_MAX(
			ocm_tile_info[tile_id].scratch_pages, scratch_pages);

		/* WB pages */
		for (page_id = wb_page_start; page_id <= wb_page_end; page_id++)
			ocm_tile_info[tile_id]
				.ocm_mask[page_id / OCM_MAP_WORD_SIZE] =
				SET_BIT(ocm_tile_info[tile_id]
						.ocm_mask[page_id /
							  OCM_MAP_WORD_SIZE],
					page_id % OCM_MAP_WORD_SIZE);

		ocm_tile_info[tile_id].last_wb_page = PLT_MAX(
			ocm_tile_info[tile_id].last_wb_page, wb_page_end);
	}

	ml_model->model_addr.tile_start = tile_start;
	ml_model->model_addr.tile_end = tile_end;

	plt_ml_dbg("tilemask = 0x%016lx, tile_start = %d, tile_end = %d",
		   tilemask, tile_start, tile_end);
	plt_ml_dbg("wb_page_start = %d, wb_page_end = %d", wb_page_start,
		   wb_page_end);
	plt_ml_dbg("scratch_page_start = %d, scratch_page_end = %d",
		   scratch_page_start, scratch_page_end);
}

static void
cnxk_ml_ocm_free_pages(struct rte_mldev *dev, uint32_t model_id)
{
	struct cn10k_ml_ocm_tile_info *ocm_tile_info;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_model *ml_model;
	struct cnxk_ml_dev *ml_dev;

	int scratch_resize_pages;
	int wb_page_start;
	int wb_page_end;
	int prev_start;
	int curr_start;
	int tile_id;
	int page_id;
	uint32_t i;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ml_model = ml_config->ml_models[model_id];
	ocm_tile_info =
		(struct cn10k_ml_ocm_tile_info *)(ml_config->ocm_tile_info);

	/* Update OCM info for WB memory */
	wb_page_start = ml_model->model_mem_map.wb_page_start;
	wb_page_end = wb_page_start + ml_model->model_mem_map.wb_pages - 1;
	for (tile_id = ml_model->model_addr.tile_start;
	     tile_id <= ml_model->model_addr.tile_end; tile_id++) {
		for (page_id = wb_page_start; page_id <= wb_page_end;
		     page_id++) {
			ocm_tile_info[tile_id]
				.ocm_mask[page_id / OCM_MAP_WORD_SIZE] =
				CLEAR_BIT(ocm_tile_info[tile_id]
						  .ocm_mask[page_id /
							    OCM_MAP_WORD_SIZE],
					  page_id % OCM_MAP_WORD_SIZE);
		}

		/* Update last_wb_page size */
		if (wb_page_end == ocm_tile_info[tile_id].last_wb_page)
			ocm_tile_info[tile_id].last_wb_page = wb_page_start - 1;

		/* Update scratch page size and clear extra bits */
		scratch_resize_pages = 0;

		/* Get max scratch pages required, excluding the current model
		 */
		for (i = 0; i < ml_config->max_models_created; i++) {
			if ((i != model_id) &&
			    (ml_config->ml_models[i] != NULL)) {
				if (IS_BIT_SET(ml_config->ml_models[i]
						       ->model_mem_map.tilemask,
					       tile_id))
					scratch_resize_pages = PLT_MAX(
						(int)ml_config->ml_models[i]
							->model_mem_map
							.scratch_pages,
						scratch_resize_pages);
			}
		}

		/* Clear extra scratch pages */
		if (scratch_resize_pages <
		    ocm_tile_info[tile_id].scratch_pages) {
			prev_start = ml_config->ocm_pages -
				     ocm_tile_info[tile_id].scratch_pages + 1;
			curr_start =
				ml_config->ocm_pages - scratch_resize_pages + 1;
			for (page_id = prev_start; page_id <= curr_start;
			     page_id++) {
				ocm_tile_info[tile_id]
					.ocm_mask[page_id / OCM_MAP_WORD_SIZE] =
					CLEAR_BIT(
						ocm_tile_info[tile_id].ocm_mask
							[page_id /
							 OCM_MAP_WORD_SIZE],
						page_id % OCM_MAP_WORD_SIZE);
			}
			ocm_tile_info[tile_id].scratch_pages =
				scratch_resize_pages;
		}
	}
}

static int
cnxk_ml_io_type_get_size(enum rte_ml_io_type type)
{
	switch (type) {
	case RTE_ML_IO_TYPE_INT8:
	case RTE_ML_IO_TYPE_UINT8:
		return sizeof(uint8_t);
	case RTE_ML_IO_TYPE_INT16:
	case RTE_ML_IO_TYPE_UINT16:
		return sizeof(uint16_t);
	case RTE_ML_IO_TYPE_INT32:
	case RTE_ML_IO_TYPE_UINT32:
		return sizeof(uint32_t);
	case RTE_ML_IO_TYPE_FP16:
		return sizeof(float) / 2;
	case RTE_ML_IO_TYPE_FP32:
		return sizeof(float);
	default:
		return -EINVAL;
	}
}

static int
cnxk_ml_metadata_check(struct cnxk_ml_model_metadata *metadata)
{
	uint8_t version[4];
	char str[PATH_MAX] = {0};

	if (strncmp((char *)metadata->metadata_header.magic,
		    ML_MODEL_MAGIC_STRING, 4) != 0) {
		plt_err("Invalid model, magic = %s",
			metadata->metadata_header.magic);
		return -1;
	}

	if (metadata->metadata_header.target_architecture !=
	    ML_MODEL_TARGET_ARCH) {
		plt_err("Model target architecture (%d) not supported",
			metadata->metadata_header.target_architecture);
		return -1;
	}

	memcpy(version, metadata->metadata_header.version, 4 * sizeof(uint8_t));
	snprintf(str, PATH_MAX, "%d.%d.%d.%d", version[0], version[1],
		 version[2], version[3]);
	if (version[0] * 1000 + version[1] * 100 < ML_MODEL_VERSION) {
		plt_err("Metadata version = %s (< %d.%d.%d.%d) not supported",
			str, (ML_MODEL_VERSION / 1000) % 10,
			(ML_MODEL_VERSION / 100) % 10,
			(ML_MODEL_VERSION / 10) % 10, ML_MODEL_VERSION % 10);
		return -1;
	}

	return 0;
}

static int
cnxk_ml_model_addr_update(struct cnxk_ml_model_metadata *model_metadata,
			  struct cnxk_ml_model_addr *model_addr, char **dmaaddr)
{
	int output_type_size, model_output_type_size;
	int input_type_size, model_input_type_size;
	uint32_t w, x, y, z;
	uint8_t i;

	/* Inputs */
	for (i = 0; i < model_metadata->model.num_input; i++) {
		input_type_size = cnxk_ml_io_type_get_size(
			model_metadata->input[i].input_type);
		if (input_type_size <= 0) {
			plt_err("input[%d] - invalid metadata, input_type = %d",
				i, model_metadata->input[i].input_type);
			return -EINVAL;
		}

		model_input_type_size = cnxk_ml_io_type_get_size(
			model_metadata->input[i].model_input_type);
		if (model_input_type_size <= 0) {
			plt_err("input[%d] - invalid metadata, model_input_type = %d",
				i, model_metadata->input[i].model_input_type);
			return -EINVAL;
		}

		if (model_metadata->input[i].relocatable != 1) {
			plt_err("input[%d] - invalid metadata, relocatable = %d",
				i, model_metadata->input[i].relocatable);
			return -EINVAL;
		}

		w = model_metadata->input[i].shape.w;
		x = model_metadata->input[i].shape.x;
		y = model_metadata->input[i].shape.y;
		z = model_metadata->input[i].shape.z;

		if (w == 0)
			w = 1;
		if (x == 0)
			x = 1;
		if (y == 0)
			y = 1;
		if (z == 0)
			z = 1;

		model_addr->input[i].sz = (w * x * y * z) * input_type_size;
		model_addr->input[i].sz_q =
			(w * x * y * z) * model_input_type_size;
		plt_ml_dbg("input[%d] - w:%d x:%d y:%d z:%d, sz = %d sz_q = %d",
			   i, w, x, y, z, model_addr->input[i].sz,
			   model_addr->input[i].sz_q);

		model_addr->input[i].addr_base = PLT_PTR_SUB(
			*dmaaddr, model_metadata->input[i].mem_offset);
		*dmaaddr = PLT_PTR_ADD(model_addr->input[i].addr_base,
				       model_metadata->input[i].mem_offset);

		model_addr->input[i].addr = *dmaaddr;
		*dmaaddr += model_addr->input[i].sz;
	}

	/* Outputs */
	for (i = 0; i < model_metadata->model.num_output; i++) {
		output_type_size = cnxk_ml_io_type_get_size(
			model_metadata->output[i].output_type);
		if (output_type_size <= 0) {
			plt_err("output[%d] - invalid metadata, output_type = %d",
				i, model_metadata->output[i].output_type);
			return -EINVAL;
		}

		model_output_type_size = cnxk_ml_io_type_get_size(
			model_metadata->output[i].model_output_type);
		if (model_output_type_size <= 0) {
			plt_err("output[%d] - invalid metadata, model_output_type = %d",
				i, model_metadata->output[i].model_output_type);
			return -EINVAL;
		}

		if (model_metadata->output[i].relocatable != 1) {
			plt_err("output[%d] - invalid metadata, relocatable = %d",
				i, model_metadata->output[i].relocatable);
			return -EINVAL;
		}

		model_addr->output[i].sz =
			model_metadata->output[i].size * output_type_size;
		model_addr->output[i].sz_q =
			model_metadata->output[i].size * model_output_type_size;

		plt_ml_dbg("output[%d] - sz = %d, sz_q = %d", i,
			   model_addr->output[i].sz,
			   model_addr->output[i].sz_q);

		model_addr->output[i].addr_base = PLT_PTR_SUB(
			*dmaaddr, model_metadata->output[i].mem_offset);
		*dmaaddr = PLT_PTR_ADD(model_addr->output[i].addr_base,
				       model_metadata->output[i].mem_offset);

		model_addr->output[i].addr = *dmaaddr;
		*dmaaddr += model_addr->output[i].sz;
	}

	return 0;
}

static void
cnxk_ml_prep_sp_job_descriptor(struct rte_mldev *dev,
			       struct cnxk_ml_model *ml_model,
			       struct cnxk_ml_jd *jd,
			       enum cnxk_ml_job_cmd job_cmd,
			       struct cnxk_ml_job_compl *ml_job_compl)
{
	struct cnxk_ml_model_metadata *model_metadata;
	struct cnxk_ml_model_addr *model_addr;
	struct cnxk_ml_dev *ml_dev;

	ml_dev = dev->data->dev_private;
	model_addr = &ml_model->model_addr;
	model_metadata = &ml_model->model_metadata;

	memset(jd, 0, sizeof(struct cnxk_ml_jd));
	jd->hdr.compl_W0.u = 0;
	jd->hdr.compl_W1.status_ptr = PLT_U64_CAST(&ml_job_compl->status_ptr);
	jd->hdr.model_id = ml_model->model_id;
	jd->hdr.command = job_cmd;
	jd->hdr.flags = ML_FLAGS_POLL_COMPL;
	jd->hdr.job_result =
		roc_ml_addr_ap2mlip(&ml_dev->roc, &ml_job_compl->job_result);

	if (job_cmd == CNXK_ML_JOB_CMD_LOAD) {
		jd->load.model_src_ddr_addr = PLT_U64_CAST(roc_ml_addr_ap2mlip(
			&ml_dev->roc, model_addr->init_load_addr));
		jd->load.model_dst_ddr_addr = PLT_U64_CAST(roc_ml_addr_ap2mlip(
			&ml_dev->roc, model_addr->init_run_addr));
		jd->load.model_init_offset = 0x0;
		jd->load.model_main_offset =
			model_metadata->init_model.file_size;
		jd->load.model_finish_offset =
			model_metadata->init_model.file_size +
			model_metadata->main_model.file_size;
		jd->load.model_init_size = model_metadata->init_model.file_size;
		jd->load.model_main_size = model_metadata->main_model.file_size;
		jd->load.model_finish_size =
			model_metadata->finish_model.file_size;
		jd->load.model_wb_offset =
			model_metadata->init_model.file_size +
			model_metadata->main_model.file_size +
			model_metadata->finish_model.file_size;
		jd->load.num_layers = model_metadata->model.num_layers;
		jd->load.num_gather_entries = 0;
		jd->load.num_scatter_entries = 0;
		jd->load.tilemask = 0; /* Updated after reserving pages */
		jd->load.ocm_wb_base_address =
			0; /* Updated after reserving pages */
		jd->load.ocm_wb_range_start =
			model_metadata->model.ocm_wb_range_start;
		jd->load.ocm_wb_range_end =
			model_metadata->model.ocm_wb_range_end;
		jd->load.ddr_wb_base_address = PLT_U64_CAST(roc_ml_addr_ap2mlip(
			&ml_dev->roc,
			PLT_PTR_ADD(model_addr->finish_load_addr,
				    model_metadata->finish_model.file_size)));
		jd->load.ddr_wb_range_start =
			model_metadata->model.ddr_wb_range_start;
		jd->load.ddr_wb_range_end =
			model_metadata->model.ddr_wb_range_end;
		jd->load.input.s.ddr_range_start =
			model_metadata->model.ddr_input_range_start;
		jd->load.input.s.ddr_range_end =
			model_metadata->model.ddr_input_range_end;
		jd->load.output.s.ddr_range_start =
			model_metadata->model.ddr_output_range_start;
		jd->load.output.s.ddr_range_end =
			model_metadata->model.ddr_output_range_end;
	}
}

static void
cnxk_ml_prep_fp_job_descriptor(struct rte_mldev *dev,
			       struct cnxk_ml_model *ml_model,
			       struct cnxk_ml_jd *jd,
			       struct cnxk_ml_job_compl *ml_job_compl,
			       struct rte_ml_op *op)
{
	struct cnxk_ml_dev *ml_dev;

	ml_dev = dev->data->dev_private;

	memset(jd, 0, sizeof(struct cnxk_ml_jd));
	jd->hdr.compl_W0.u = 0;
	jd->hdr.compl_W1.W1 = 0;

	jd->hdr.compl_W1.status_ptr = PLT_U64_CAST(&ml_job_compl->status_ptr);
	jd->hdr.model_id = ml_model->model_id;
	jd->hdr.command = CNXK_ML_JOB_CMD_RUN;
	jd->hdr.flags = ML_FLAGS_POLL_COMPL;
	jd->hdr.job_result =
		roc_ml_addr_ap2mlip(&ml_dev->roc, &ml_job_compl->job_result);
	jd->run.input_ddr_addr =
		PLT_U64_CAST(roc_ml_addr_ap2mlip(&ml_dev->roc, op->ibuffer));
	jd->run.output_ddr_addr =
		PLT_U64_CAST(roc_ml_addr_ap2mlip(&ml_dev->roc, op->obuffer));
}

static void
cnxk_ml_model_info_set(struct cnxk_ml_model *ml_model)
{
	struct cnxk_ml_model_metadata *model_metadata;
	struct rte_ml_output_info *output_info;
	struct cnxk_ml_model_addr *model_addr;
	struct rte_ml_input_info *input_info;
	struct rte_ml_model_info *model_info;
	uint8_t i;

	model_info = (struct rte_ml_model_info *)(ml_model->model_info);
	input_info = PLT_PTR_ADD(model_info, sizeof(struct rte_ml_model_info));
	output_info = PLT_PTR_ADD(input_info,
				  ml_model->model_metadata.model.num_input *
					  sizeof(struct rte_ml_input_info));

	model_metadata = &ml_model->model_metadata;
	model_addr = &ml_model->model_addr;

	/* Set model info */
	memset(model_info, 0, sizeof(struct rte_ml_model_info));
	memcpy(model_info->name, model_metadata->model.name, ML_MODEL_NAME_LEN);
	memcpy(model_info->version, model_metadata->model.version, 4);
	model_info->index = ml_model->model_id;
	model_info->num_inputs = model_metadata->model.num_input;
	model_info->total_input_size = 0;
	model_info->input_info = input_info;
	model_info->num_outputs = model_metadata->model.num_output;
	model_info->total_output_size = 0;
	model_info->output_info = output_info;

	/* Set input info */
	for (i = 0; i < model_info->num_inputs; i++) {
		memcpy(input_info[i].name, model_metadata->input[i].input_name,
		       ML_INPUT_NAME_LEN);
		input_info[i].model_input_type =
			model_metadata->input[i].model_input_type;
		input_info[i].shape.format =
			model_metadata->input[i].shape.format;
		input_info[i].shape.w = model_metadata->input[i].shape.w;
		input_info[i].shape.x = model_metadata->input[i].shape.x;
		input_info[i].shape.y = model_metadata->input[i].shape.y;
		input_info[i].shape.z = model_metadata->input[i].shape.z;
		input_info[i].size = model_addr->input[i].sz_q;

		model_info->total_input_size += model_addr->input[i].sz_q;
	}

	/* Set output info */
	for (i = 0; i < model_info->num_outputs; i++) {
		memcpy(output_info[i].name,
		       model_metadata->output[i].output_name,
		       ML_OUTPUT_NAME_LEN);
		output_info[i].model_output_type =
			model_metadata->output[i].model_output_type;
		output_info[i].shape.format = RTE_ML_IO_FORMAT_NCHW;
		output_info[i].shape.w = model_metadata->output[i].size;
		output_info[i].shape.x = 1;
		output_info[i].shape.y = 1;
		output_info[i].shape.z = 1;
		output_info[i].size = model_addr->output[i].sz_q;

		model_info->total_output_size += model_addr->output[i].sz_q;
	}
}

int
cn10k_ml_dev_configure(struct rte_mldev *dev, struct rte_mldev_config *conf)
{
	struct cn10k_ml_ocm_tile_info *ocm_tile_info;
	struct cnxk_ml_config *ml_config;
	const struct plt_memzone *mz;
	uint64_t ocm_tile_info_size;
	struct cnxk_ml_dev *ml_dev;
	struct cnxk_ml_fw *ml_fw;
	uint64_t ml_models_size;
	uint64_t scale_factor;
	uint64_t reg_val64;
	uint32_t model_id;
	uint16_t tile_id;
	uint64_t mz_size;
	uint64_t fw_size;
	void *fw_buffer;
	int ret = 0;

	if (dev == NULL || conf == NULL)
		return -EINVAL;

	/* Update device reference in firmware and set handles */
	ml_dev = dev->data->dev_private;
	ml_fw = &ml_dev->ml_fw;
	ml_fw->ml_dev = ml_dev;

	/* Reserve memzone for firmware load completion and data */
	mz_size = sizeof(struct cnxk_ml_fw_load_compl) + FW_BUFFER_SIZE;
	mz = plt_memzone_reserve_aligned(FW_MEMZONE_NAME, mz_size, 0,
					 ML_CN10K_ALIGN_SIZE);
	if (mz == NULL) {
		plt_err("plt_memzone_reserve failed : %s", FW_MEMZONE_NAME);
		return -ENOMEM;
	}
	ml_fw->load_fw = mz->addr;
	ml_fw->data_fw =
		PLT_PTR_ADD(mz->addr, sizeof(struct cnxk_ml_fw_load_compl));

	/* Reset firmware load completion */
	memset(ml_fw->load_fw, 0, sizeof(struct cnxk_ml_fw_load_compl));
	memset(&ml_fw->load_fw->version[0], '\0', ML_FW_VERSION_STRLEN);

	/* Enable ML device */
	reg_val64 = (ROC_ML_CFG_JD_SIZE | ROC_ML_CFG_MLIP_ENA);
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);

	/* Load firmware */
	if (roc_env_is_emulator() || roc_env_is_hw()) {
		/* Read firmware binary to a local buffer */
		ret = rte_firmware_read(ml_fw->filepath, &fw_buffer, &fw_size);
		if (ret < 0) {
			plt_err("Can't read firmware data: %s\n",
				ml_fw->filepath);
			goto err_exit;
		}

		/* Load firmware */
		ret = cn10k_ml_fw_load_cn10ka(ml_fw, fw_buffer, fw_size);
		free(fw_buffer);
		if (ret != 0)
			goto err_exit;
	} else {
		if (cn10k_ml_fw_load_asim(ml_fw) == -1)
			goto err_exit;
	}

	ml_config = &ml_dev->ml_config;
	ml_config->ml_dev = ml_dev;
	ml_config->max_models_created = ML_CN10K_MAX_MODELS;

	/* Reserve memzone for configuration data and update ml_config */
	ml_models_size = PLT_ALIGN_CEIL(sizeof(struct cnxk_ml_model *) *
						ML_CN10K_MAX_MODELS,
					ML_CN10K_ALIGN_SIZE);
	ocm_tile_info_size = PLT_ALIGN_CEIL(
		sizeof(struct cn10k_ml_ocm_tile_info) * ML_CN10K_OCM_NUMTILES,
		ML_CN10K_ALIGN_SIZE);
	mz_size = ml_models_size + ocm_tile_info_size;
	mz = plt_memzone_reserve_aligned(ML_CONFIG_MEMZONE_NAME, mz_size, 0,
					 ML_CN10K_ALIGN_SIZE);
	if (mz == NULL) {
		plt_err("plt_memzone_reserve failed : %s",
			ML_CONFIG_MEMZONE_NAME);
		goto err_exit;
	}

	ml_config->ml_models = mz->addr;
	ml_config->ocm_tile_info =
		PLT_PTR_ADD(ml_config->ml_models, ml_models_size);
	ml_config->max_models_created = ML_CN10K_MAX_MODELS;
	ml_config->ocm_num_tiles = ML_CN10K_OCM_NUMTILES;
	ml_config->ocm_size = ML_CN10K_OCM_TILESIZE;
	ml_config->ocm_page_size = ML_CN10K_OCM_PAGESIZE;
	ml_config->ocm_pages = ml_config->ocm_size / ml_config->ocm_page_size;
	ml_config->ocm_mask_words =
		ml_config->ocm_pages / (8 * sizeof(uint8_t));

	for (model_id = 0; model_id < ml_config->max_models_created; model_id++)
		ml_config->ml_models[model_id] = NULL;

	ocm_tile_info =
		(struct cn10k_ml_ocm_tile_info *)(ml_config->ocm_tile_info);
	for (tile_id = 0; tile_id < ml_config->ocm_num_tiles; tile_id++)
		ocm_tile_info[tile_id].last_wb_page = -1;

	/* Internal Job completion pool. Used for sync and poll mode jobs */
	ml_config->job_pool =
		rte_mempool_create(ML_JOBPOOL_NAME, ML_JOBPOOL_SIZE,
				   sizeof(struct cnxk_ml_job_compl), 0, 0, NULL,
				   NULL, NULL, NULL, rte_socket_id(), 0);
	if (ml_config->job_pool == NULL) {
		plt_err("Job pool creation failed : %s", ML_JOBPOOL_NAME);
		ret = -1;
		goto err_exit;
	}

	rte_spinlock_init(&ml_config->lock);

	/* Set timeouts */
	if (roc_env_is_asim())
		scale_factor = ML_TIMEOUT_MULTIPLIER * plt_tsc_hz();
	else
		scale_factor = plt_tsc_hz();

	ml_config->timeout.load = ML_TIMEOUT_LOAD_S * scale_factor;
	ml_config->timeout.unload = ML_TIMEOUT_UNLOAD_S * scale_factor;
	ml_config->timeout.run = ML_TIMEOUT_RUN_S * scale_factor;

	/* Configure queue-pairs */
	if (dev->data->queue_pairs == NULL) {
		mz_size = sizeof(dev->data->queue_pairs[0]) *
			  conf->nb_queue_pairs;
		dev->data->queue_pairs = rte_zmalloc(
			"mldev->queue_pairs", mz_size, RTE_CACHE_LINE_SIZE);
		if (dev->data->queue_pairs == NULL) {
			dev->data->nb_queue_pairs = 0;
			plt_err("Failed to get memory for queue_pairs, nb_queue_pairs %u",
				conf->nb_queue_pairs);
			return -ENOMEM;
		}
	} else { /* re-configure */
		void **queue_pairs;
		uint16_t i;

		for (i = conf->nb_queue_pairs; i < dev->data->nb_queue_pairs;
		     i++) {
			ret = cn10k_ml_queue_pair_release(dev, i);
			if (ret < 0)
				return ret;
		}

		queue_pairs = dev->data->queue_pairs;
		queue_pairs = rte_realloc(queue_pairs,
					  sizeof(queue_pairs[0]) *
						  conf->nb_queue_pairs,
					  RTE_CACHE_LINE_SIZE);
		if (queue_pairs == NULL) {
			plt_err("Failed to realloc queue_pairs, nb_queue_pairs = %u",
				conf->nb_queue_pairs);
			return -ENOMEM;
		}

		memset(queue_pairs, 0,
		       sizeof(queue_pairs[0]) * conf->nb_queue_pairs);
		dev->data->queue_pairs = queue_pairs;
	}
	dev->data->nb_queue_pairs = conf->nb_queue_pairs;

	dev->enqueue_burst = cn10k_ml_enqueue_burst;
	dev->dequeue_burst = cn10k_ml_dequeue_burst;
	dev->inference_sync = cn10k_ml_inference_sync;

	ml_config->active = true;

	return 0;

err_exit:
	/* Clear resources */
	mz = plt_memzone_lookup(ML_CONFIG_MEMZONE_NAME);
	if (mz != NULL)
		plt_memzone_free(mz);

	mz = plt_memzone_lookup(FW_MEMZONE_NAME);
	if (mz != NULL)
		plt_memzone_free(mz);

	/* Disable device */
	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_CFG);
	reg_val64 &= ~ROC_ML_CFG_MLIP_ENA;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);

	return ret;
}

int
cn10k_ml_dev_close(struct rte_mldev *dev)
{
	struct cnxk_ml_config *ml_config;
	const struct plt_memzone *mz;
	struct rte_mempool *job_pool;
	struct cnxk_ml_dev *ml_dev;
	int ret = 0;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;

	/* Set config inactive */
	ml_config->active = false;

	/* Set MLIP clock off and stall on */
	roc_ml_clk_force_off(&ml_dev->roc);
	roc_ml_dma_stall_on(&ml_dev->roc);
	ret = roc_ml_mlip_reset(&ml_dev->roc, false);

	/* Clear scratch registers */
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_WORK_PTR);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_AP_FW_COMM);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_DBG_BUFFER_HEAD_C0);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_DBG_BUFFER_TAIL_C0);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_DBG_BUFFER_HEAD_C1);
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_SCRATCH_DBG_BUFFER_TAIL_C1);

	/* Reset ML_MLR_BASE */
	roc_ml_reg_write64(&ml_dev->roc, 0, ML_MLR_BASE);
	plt_ml_dbg("ML_MLR_BASE = 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_MLR_BASE));

	/* Destroy poll mode job pool*/
	job_pool = rte_mempool_lookup(ML_JOBPOOL_NAME);
	if (job_pool != NULL)
		rte_mempool_free(job_pool);

	if (dev->data->queue_pairs)
		rte_free(dev->data->queue_pairs);

	/* Clear resources */
	mz = plt_memzone_lookup(ML_CONFIG_MEMZONE_NAME);
	if (mz != NULL)
		plt_memzone_free(mz);

	mz = plt_memzone_lookup(FW_MEMZONE_NAME);
	if (mz)
		plt_memzone_free(mz);

	return ret;
}

int
cn10k_ml_dev_start(struct rte_mldev *dev)
{
	struct cnxk_ml_dev *ml_dev;
	uint64_t reg_val64;

	ml_dev = dev->data->dev_private;

	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_CFG);
	reg_val64 |= ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_CFG));

	return 0;
}

void
cn10k_ml_dev_stop(struct rte_mldev *dev)
{
	struct cnxk_ml_dev *ml_dev;
	uint64_t reg_val64;

	ml_dev = dev->data->dev_private;

	reg_val64 = roc_ml_reg_read64(&ml_dev->roc, ML_CFG);
	reg_val64 &= ~ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&ml_dev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx",
		   roc_ml_reg_read64(&ml_dev->roc, ML_CFG));
}

static int
cn10k_ml_dev_info_get(struct rte_mldev *dev, struct rte_mldev_info *info)
{
	if (info == NULL)
		return -EINVAL;

	info->max_nb_queue_pairs = ML_CN10K_QP_PER_DEVICE;
	info->driver_id = cn10k_mldev_driver_id;
	info->driver_name = dev->device->driver->name;
	info->device = dev->device;

	return 0;
}

int
cn10k_ml_model_create(struct rte_mldev *dev, struct rte_ml_model *model,
		      uint8_t *model_id)
{
	struct cnxk_ml_model_metadata *model_metadata;
	struct cnxk_ml_model_metadata metadata;
	struct cnxk_ml_model_addr *model_addr;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_model *ml_model;
	struct cnxk_ml_dev *ml_dev;

	const struct plt_memzone *mz;
	char str[PATH_MAX] = {0};
	uint64_t mz_size;
	uint8_t *buffer;
	uint8_t idx;

	uint8_t *base_dma_addr_load;
	uint8_t *base_dma_addr_run;
	size_t model_info_size;
	size_t model_data_size;
	uint8_t *base_dma_addr;
	uint8_t *dma_addr_load;
	uint8_t *dma_addr_run;
	uint8_t *wb_load_addr;
	int blobsz;
	int fpos;

	uint16_t scratch_pages;
	uint64_t scratch_size;
	uint16_t wb_pages;
	uint64_t wb_size;

	PLT_ASSERT(model != NULL);
	PLT_ASSERT(model_id != NULL);

	buffer = model->model_buffer;
	memcpy(&metadata, buffer, sizeof(struct cnxk_ml_model_metadata));
	if (cnxk_ml_metadata_check(&metadata) != 0)
		return -1;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;

	/* Assign model ID */
	for (idx = 0; idx < ml_config->max_models_created; idx++) {
		if (ml_config->ml_models[idx] == NULL)
			break;
	}

	if (idx >= ml_config->max_models_created) {
		plt_err("No slots available to load new model");
		return -1;
	}

	/* Get MZ size */
	model_info_size =
		sizeof(struct rte_ml_model_info) +
		metadata.model.num_input * sizeof(struct rte_ml_input_info) +
		metadata.model.num_output * sizeof(struct rte_ml_output_info);
	model_info_size = PLT_ALIGN_CEIL(model_info_size, ML_CN10K_ALIGN_SIZE);

	model_data_size = metadata.init_model.file_size +
			  metadata.main_model.file_size +
			  metadata.finish_model.file_size +
			  metadata.weights_bias.file_size;
	model_data_size = PLT_ALIGN_CEIL(model_data_size, ML_CN10K_ALIGN_SIZE);
	mz_size = PLT_ALIGN_CEIL(sizeof(struct cnxk_ml_model),
				 ML_CN10K_ALIGN_SIZE) +
		  model_info_size + 2 * model_data_size;

	/* Allocate memzone for model object and model data */
	snprintf(str, PATH_MAX, "%s_%d", ML_MODEL_MEMZONE_NAME, idx);
	mz = plt_memzone_reserve_aligned(str, mz_size, 0, ML_CN10K_ALIGN_SIZE);
	if (!mz) {
		plt_err("plt_memzone_reserve failed : %s", str);
		goto err_exit;
	}

	ml_model = mz->addr;
	ml_model->ml_config = ml_config;
	ml_model->model_id = idx;
	ml_model->model_info = PLT_PTR_ADD(
		mz->addr, PLT_ALIGN_CEIL(sizeof(struct cnxk_ml_model),
					 ML_CN10K_ALIGN_SIZE));
	if (model->model_name) {
		plt_strlcpy(ml_model->name, model->model_name,
			    sizeof(ml_model->name));
		plt_ml_dbg("ml_model->name = %s", ml_model->name);
	}

	model_addr = &ml_model->model_addr;
	model_metadata = &ml_model->model_metadata;
	memcpy(model_metadata, &metadata,
	       sizeof(struct cnxk_ml_model_metadata));

	wb_size = model_metadata->model.ocm_wb_range_end -
		  model_metadata->model.ocm_wb_range_start + 1;
	if (wb_size % ml_config->ocm_page_size)
		wb_pages = wb_size / ml_config->ocm_page_size + 1;
	else
		wb_pages = wb_size / ml_config->ocm_page_size;
	plt_ml_dbg("wb_size = %" PRIu64 ", wb_pages = %" PRIu16, wb_size,
		   wb_pages);

	scratch_size =
		ml_config->ocm_size - model_metadata->model.ocm_tmp_range_floor;
	if (model_metadata->model.ocm_tmp_range_floor %
	    ml_config->ocm_page_size)
		scratch_pages = scratch_size / ml_config->ocm_page_size + 1;
	else
		scratch_pages = scratch_size / ml_config->ocm_page_size;
	plt_ml_dbg("scratch_size = %" PRIu64 ", scratch_pages = %" PRIu16,
		   scratch_size, scratch_pages);

	/* Check if the model can be loaded on OCM */
	if ((wb_pages + scratch_pages) > ML_CN10K_OCM_NUMPAGES) {
		plt_err("Cannot create model, OCM pages required = %d (> %d)",
			wb_pages + scratch_pages, ML_CN10K_OCM_NUMPAGES);
		goto err_exit;
	}

	/* Set DMA base address */
	base_dma_addr = PLT_PTR_ADD(mz->addr,
				    PLT_ALIGN_CEIL(sizeof(struct cnxk_ml_model),
						   ML_CN10K_ALIGN_SIZE) +
					    model_info_size);
	base_dma_addr_load = base_dma_addr;
	base_dma_addr_run = base_dma_addr + model_data_size;
	dma_addr_load = base_dma_addr_load;
	dma_addr_run = base_dma_addr_run;

	/* Init Section */
	fpos = sizeof(struct cnxk_ml_model_metadata);
	blobsz = model_metadata->init_model.file_size;
	if (blobsz <= 0) {
		plt_err("Invalid metadata, init_model.file_size = %d", blobsz);
		goto err_exit;
	}
	model_addr->init_load_addr = dma_addr_load;
	model_addr->init_run_addr = dma_addr_run;
	memcpy(dma_addr_load, buffer + fpos, blobsz);

	/* Main Section */
	dma_addr_load += blobsz;
	dma_addr_run += blobsz;
	fpos += blobsz;
	blobsz = model_metadata->main_model.file_size;
	if (blobsz <= 0) {
		plt_err("Invalid metadata, main_model.file_size = %d", blobsz);
		goto err_exit;
	}
	model_addr->main_load_addr = dma_addr_load;
	model_addr->main_run_addr = dma_addr_run;
	memcpy(dma_addr_load, buffer + fpos, blobsz);

	/* Finish Section */
	dma_addr_load += blobsz;
	dma_addr_run += blobsz;
	fpos += blobsz;
	blobsz = model_metadata->finish_model.file_size;
	if (blobsz <= 0) {
		plt_err("Invalid metadata, finish_model.file_size = %d",
			blobsz);
		goto err_exit;
	}
	model_addr->finish_load_addr = dma_addr_load;
	model_addr->finish_run_addr = dma_addr_run;
	memcpy(dma_addr_load, buffer + fpos, blobsz);

	/* Weights & Bias Section*/
	dma_addr_load += blobsz;
	dma_addr_run += blobsz;
	fpos += blobsz;
	blobsz = model_metadata->weights_bias.file_size;
	if (blobsz <= 0) {
		plt_err("Invalid metadata, weights_bias.file_size = %d",
			blobsz);
		goto err_exit;
	}
	if (model_metadata->weights_bias.relocatable == 1) {
		model_addr->wb_base_addr = PLT_PTR_SUB(
			dma_addr_load, model_metadata->weights_bias.mem_offset);
		dma_addr_load =
			PLT_PTR_ADD(model_addr->wb_base_addr,
				    model_metadata->weights_bias.mem_offset);
		wb_load_addr = dma_addr_load;
		dma_addr_load += blobsz;
		plt_ml_dbg("wb_load_addr = 0x%016lx", (uint64_t)wb_load_addr);
	} else {
		plt_err("Non-relocatable models not supported");
		goto err_exit;
	}
	model_addr->wb_load_addr = wb_load_addr;
	memcpy(wb_load_addr, buffer + fpos, blobsz);

	dma_addr_load += blobsz;
	if (cnxk_ml_model_addr_update(model_metadata, model_addr,
				      (char **)dma_addr_load) < 0)
		goto err_exit;

	/* Copy data from load to run. run address to be used by MLIP */
	memcpy(base_dma_addr_run, base_dma_addr_load, model_data_size);

	memset(&ml_model->model_mem_map, 0,
	       sizeof(struct cnxk_ml_ocm_model_map));
	ml_model->model_mem_map.ocm_reserved = false;
	ml_model->model_mem_map.tilemask = 0;
	ml_model->model_mem_map.wb_page_start = -1;
	ml_model->model_mem_map.wb_pages = wb_pages;
	ml_model->model_mem_map.scratch_pages = scratch_pages;

	cnxk_ml_model_info_set(ml_model);

	/* Initialize model lock and state */
	plt_spinlock_init(&ml_model->lock);
	ml_model->state = CNXK_ML_MODEL_STATE_CREATED;
	ml_config->ml_models[idx] = ml_model;

	plt_ml_dbg("model = 0x%016lx", PLT_U64_CAST(ml_model));
	*model_id = idx;
	return 0;

err_exit:
	snprintf(str, PATH_MAX, "%s_%d", ML_MODEL_MEMZONE_NAME, idx);
	mz = plt_memzone_lookup(str);
	if (mz)
		plt_memzone_free(mz);

	return -1;
}

int
cn10k_ml_model_destroy(struct rte_mldev *dev, uint8_t model_id)
{
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_dev *ml_dev;
	struct cnxk_ml_model *ml_model;
	char str[PATH_MAX] = {0};

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ml_model = ml_config->ml_models[model_id];

	if (ml_model->state != CNXK_ML_MODEL_STATE_CREATED) {
		plt_err("Cannot destroy. Model in use.");
		return -EBUSY;
	}

	ml_config->ml_models[model_id] = NULL;

	snprintf(str, PATH_MAX, "%s_%d", ML_MODEL_MEMZONE_NAME, model_id);
	return plt_memzone_free(plt_memzone_lookup(str));
}

int
cn10k_ml_model_load(struct rte_mldev *dev, uint8_t model_id)
{
	struct cnxk_ml_job_compl *ml_job_compl;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_model *ml_model;
	struct cnxk_ml_dev *ml_dev;
	struct cnxk_ml_jd *jd;

	bool job_enqueued;
	bool job_dequeued;
	uint8_t num_tiles;
	uint64_t tilemask;
	int wb_page_start;
	int tile_start;
	int tile_end;
	bool locked;
	int ret = 0;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ml_model = ml_config->ml_models[model_id];

	ret = rte_mempool_get(ml_config->job_pool, (void **)(&ml_job_compl));
	if (ret != 0)
		return ret;

	/* Prepare JD */
	jd = &ml_job_compl->jd;
	cnxk_ml_prep_sp_job_descriptor(dev, ml_model, jd, CNXK_ML_JOB_CMD_LOAD,
				       ml_job_compl);

	ml_job_compl->job_result.status = ML_STATUS_FAILURE;
	ml_job_compl->job_result.user_ptr = NULL;

	plt_write64(ML_CN10K_POLL_JOB_START, &ml_job_compl->status_ptr);
	plt_wmb();

	num_tiles = ml_model->model_metadata.model.tile_end -
		    ml_model->model_metadata.model.tile_start + 1;

	locked = false;
	while (!locked) {
		if (plt_spinlock_trylock(&ml_model->lock) != 0) {
			if (ml_model->state == CNXK_ML_MODEL_STATE_LOADED) {
				plt_ml_dbg(
					"Model already loaded, model = 0x%016lx",
					PLT_U64_CAST(ml_model));
				plt_spinlock_unlock(&ml_model->lock);
				return 1;
			}

			if (ml_model->state == CNXK_ML_MODEL_STATE_JOB_ACTIVE) {
				plt_err("A slow-path job is active for the model = 0x%016lx",
					PLT_U64_CAST(ml_model));
				plt_spinlock_unlock(&ml_model->lock);
				return -EBUSY;
			}

			ml_model->state = CNXK_ML_MODEL_STATE_JOB_ACTIVE;
			plt_spinlock_unlock(&ml_model->lock);
			locked = true;
		}
	}

	while (!ml_model->model_mem_map.ocm_reserved) {
		if (plt_spinlock_trylock(&ml_config->lock) != 0) {
			wb_page_start = cnxk_ml_ocm_tilemask_find(
				dev, num_tiles,
				ml_model->model_mem_map.wb_pages,
				ml_model->model_mem_map.scratch_pages, -1,
				&tilemask);

			if (wb_page_start == -1) {
				plt_err("Free pages not available on OCM tiles");
				plt_err("Failed to load model id = 0x%016lx, name = %s",
					PLT_U64_CAST(ml_model),
					ml_model->model_metadata.model.name);

				plt_spinlock_unlock(&ml_config->lock);
				return -ENOMEM;
			}

			ml_model->model_mem_map.tilemask = tilemask;
			ml_model->model_mem_map.wb_page_start = wb_page_start;

			cnxk_ml_ocm_reserve_pages(
				dev, ml_model->model_id,
				ml_model->model_mem_map.tilemask,
				ml_model->model_mem_map.wb_page_start,
				ml_model->model_mem_map.wb_pages,
				ml_model->model_mem_map.scratch_pages);
			ml_model->model_mem_map.ocm_reserved = true;
			plt_spinlock_unlock(&ml_config->lock);
		}
	}

	/* Update JD */
	cnxk_ml_ocm_tilecount(ml_model->model_mem_map.tilemask, &tile_start,
			      &tile_end);
	jd->load.tilemask = GENMASK_ULL(tile_end, tile_start);
	jd->load.ocm_wb_base_address = ml_model->model_mem_map.wb_page_start *
				       ml_config->ocm_page_size;

	job_enqueued = false;
	job_dequeued = false;
	do {
		if (!job_enqueued) {
			ml_job_compl->start_cycle = plt_tsc_cycles();
			job_enqueued = roc_ml_scratch_enqueue(&ml_dev->roc, jd);
		}

		if (job_enqueued && !job_dequeued)
			job_dequeued = roc_ml_scratch_dequeue(&ml_dev->roc, jd);

		if (job_dequeued)
			break;
	} while (plt_tsc_cycles() - ml_job_compl->start_cycle <
		 ml_config->timeout.load);

	if (job_dequeued) {
		if (plt_read64(&ml_job_compl->status_ptr) ==
		    ML_CN10K_POLL_JOB_FINISH) {
			if (ml_job_compl->job_result.status ==
			    ML_STATUS_SUCCESS)
				ret = 0;
			else if (ml_job_compl->job_result.status ==
				 ML_STATUS_FAILURE)
				ret = -1;
		}
	} else { /* Reset scratch registers */
		roc_ml_scratch_queue_reset(&ml_dev->roc);
		ret = -ETIME;
	}

	locked = false;
	while (!locked) {
		if (plt_spinlock_trylock(&ml_model->lock) != 0) {
			if (ret == 0)
				ml_model->state = CNXK_ML_MODEL_STATE_LOADED;
			else
				ml_model->state = CNXK_ML_MODEL_STATE_UNKNOWN;

			plt_spinlock_unlock(&ml_model->lock);
			locked = true;
		}
	}

	if (ml_model->state == CNXK_ML_MODEL_STATE_UNKNOWN) {
		while (ml_model->model_mem_map.ocm_reserved) {
			if (plt_spinlock_trylock(&ml_config->lock) != 0) {
				cnxk_ml_ocm_free_pages(dev, ml_model->model_id);
				ml_model->model_mem_map.ocm_reserved = false;
				plt_spinlock_unlock(&ml_config->lock);
			}
		}
	}

	rte_mempool_put(ml_config->job_pool, ml_job_compl);

	return ret;
}

int
cn10k_ml_model_unload(struct rte_mldev *dev, uint8_t model_id)
{
	struct cnxk_ml_job_compl *ml_job_compl;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_model *ml_model;
	struct cnxk_ml_dev *ml_dev;
	struct cnxk_ml_jd *jd;

	bool job_enqueued;
	bool job_dequeued;
	bool locked;
	int ret = 0;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ml_model = ml_config->ml_models[model_id];

	ret = rte_mempool_get(ml_config->job_pool, (void **)(&ml_job_compl));
	if (ret != 0)
		return ret;

	/* Prepare JD */
	jd = &ml_job_compl->jd;
	cnxk_ml_prep_sp_job_descriptor(dev, ml_model, jd,
				       CNXK_ML_JOB_CMD_UNLOAD, ml_job_compl);

	ml_job_compl->job_result.status = ML_STATUS_FAILURE;
	ml_job_compl->job_result.user_ptr = NULL;

	plt_write64(ML_CN10K_POLL_JOB_START, &ml_job_compl->status_ptr);
	plt_wmb();

	locked = false;
	while (!locked) {
		if (plt_spinlock_trylock(&ml_model->lock) != 0) {
			if (ml_model->state == CNXK_ML_MODEL_STATE_CREATED) {
				plt_ml_dbg("Model not loaded, model = 0x%016lx",
					   PLT_U64_CAST(ml_model));
				plt_spinlock_unlock(&ml_model->lock);
				return 1;
			}

			if (ml_model->state == CNXK_ML_MODEL_STATE_JOB_ACTIVE) {
				plt_err("A slow-path job is active for the model = 0x%016lx",
					PLT_U64_CAST(ml_model));
				plt_spinlock_unlock(&ml_model->lock);
				return -EBUSY;
			}

			ml_model->state = CNXK_ML_MODEL_STATE_JOB_ACTIVE;
			plt_spinlock_unlock(&ml_model->lock);
			locked = true;
		}
	}

	while (ml_model->model_mem_map.ocm_reserved) {
		if (plt_spinlock_trylock(&ml_config->lock) != 0) {
			cnxk_ml_ocm_free_pages(dev, ml_model->model_id);
			ml_model->model_mem_map.ocm_reserved = false;
			plt_spinlock_unlock(&ml_config->lock);
		}
	}

	job_enqueued = false;
	job_dequeued = false;
	do {
		if (!job_enqueued) {
			ml_job_compl->start_cycle = plt_tsc_cycles();
			job_enqueued = roc_ml_scratch_enqueue(&ml_dev->roc, jd);
		}

		if (job_enqueued && !job_dequeued)
			job_dequeued = roc_ml_scratch_dequeue(&ml_dev->roc, jd);

		if (job_dequeued)
			break;
	} while (plt_tsc_cycles() - ml_job_compl->start_cycle <
		 ml_config->timeout.unload);

	if (job_dequeued) {
		if (plt_read64(&ml_job_compl->status_ptr) ==
		    ML_CN10K_POLL_JOB_FINISH) {
			if (ml_job_compl->job_result.status ==
			    ML_STATUS_SUCCESS)
				ret = 0;
			else if (ml_job_compl->job_result.status ==
				 ML_STATUS_FAILURE)
				ret = -1;
		}
	} else {
		roc_ml_scratch_queue_reset(&ml_dev->roc);
		ret = -ETIME;
	}

	locked = false;
	while (!locked) {
		if (plt_spinlock_trylock(&ml_model->lock) != 0) {
			ml_model->state = CNXK_ML_MODEL_STATE_CREATED;
			plt_spinlock_unlock(&ml_model->lock);
			locked = true;
		}
	}

	rte_mempool_put(ml_config->job_pool, ml_job_compl);

	return ret;
}

static int
cn10k_ml_model_info_get(struct rte_mldev *dev, uint8_t model_id,
			struct rte_ml_model_info *info)
{
	struct rte_ml_model_info *model_info;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_model *ml_model;
	struct cnxk_ml_dev *ml_dev;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ml_model = ml_config->ml_models[model_id];

	model_info = PLT_PTR_CAST(ml_model->model_info);
	memcpy(info->name, model_info->name, sizeof(info->name));
	memcpy(info->version, model_info->version, sizeof(info->version));
	info->index = model_info->index;
	info->num_inputs = model_info->num_inputs;
	info->total_input_size = model_info->total_input_size;
	info->num_outputs = model_info->num_outputs;
	info->total_output_size = model_info->total_output_size;

	if (info->input_info != NULL)
		memcpy(info->input_info, model_info->input_info,
		       model_info->num_inputs *
			       sizeof(struct rte_ml_input_info));

	if (info->output_info != NULL)
		memcpy(info->output_info, model_info->output_info,
		       model_info->num_outputs *
			       sizeof(struct rte_ml_output_info));

	return 0;
}

static void
qp_memzone_name_get(char *name, int size, int dev_id, int qp_id)
{
	snprintf(name, size, "cn10k_ml_pq_mem_%u:%u", dev_id, qp_id);
}

static struct cn10k_ml_qp *
cn10k_ml_qp_create(const struct rte_mldev *dev, uint16_t qp_id,
		   uint32_t nb_desc, int socket_id)
{
	const struct rte_memzone *qp_mem;
	char name[RTE_MEMZONE_NAMESIZE];
	struct cn10k_ml_qp *qp;
	uint32_t len;
	uint8_t *va;

	/* Allocate queue pair */
	qp = rte_zmalloc_socket("CNXK ML PMD Queue Pair",
				sizeof(struct cn10k_ml_qp), ROC_ALIGN,
				socket_id);
	if (qp == NULL) {
		plt_err("Could not allocate queue pair");
		return NULL;
	}

	/* For pending queue */
	len = nb_desc * sizeof(struct cn10k_ml_req);

	qp_memzone_name_get(name, RTE_MEMZONE_NAMESIZE, dev->data->dev_id,
			    qp_id);
	qp_mem = rte_memzone_reserve_aligned(name, len, socket_id,
					     RTE_MEMZONE_SIZE_HINT_ONLY |
						     RTE_MEMZONE_256MB,
					     RTE_CACHE_LINE_SIZE);
	if (qp_mem == NULL) {
		plt_err("Could not allocate reserved memzone");
		goto qp_free;
	}

	va = qp_mem->addr;
	memset(va, 0, len);

	/* Initialize pending queue */
	qp->id = qp_id;
	qp->pend_q.pending_count = 0;
	qp->pend_q.req_queue = (struct cn10k_ml_req *)va;
	qp->pend_q.enq_tail = 0;
	qp->pend_q.deq_head = 0;
	qp->nb_desc = nb_desc;

	return qp;

qp_free:
	rte_free(qp);

	return NULL;
}

int
cn10k_ml_queue_pair_setup(struct rte_mldev *dev, uint16_t qp_id,
			  const struct rte_mldev_qp_conf *qp_conf,
			  int socket_id)
{
	struct cn10k_ml_qp *qp;

	if (dev->data->queue_pairs[qp_id] != NULL)
		cn10k_ml_queue_pair_release(dev, qp_id);

	if (qp_conf->nb_desc > ML_CN10K_DESC_PER_QP) {
		plt_err("Could not setup queue pair for %u descriptors",
			qp_conf->nb_desc);
		return -EINVAL;
	}

	qp = cn10k_ml_qp_create(dev, qp_id, qp_conf->nb_desc, socket_id);
	if (qp == NULL) {
		plt_err("Could not create queue pair %d", qp_id);
		return -ENOMEM;
	}
	dev->data->queue_pairs[qp_id] = qp;

	return 0;
}

static int
cn10k_ml_qp_destroy(const struct rte_mldev *dev, struct cn10k_ml_qp *qp)
{
	const struct rte_memzone *qp_mem;
	char name[RTE_MEMZONE_NAMESIZE];
	int ret;

	qp_memzone_name_get(name, RTE_MEMZONE_NAMESIZE, dev->data->dev_id,
			    qp->id);
	qp_mem = rte_memzone_lookup(name);
	ret = rte_memzone_free(qp_mem);
	if (ret)
		return ret;

	rte_free(qp);

	return 0;
}

int
cn10k_ml_queue_pair_release(struct rte_mldev *dev, uint16_t qp_id)
{
	struct cn10k_ml_qp *qp;
	int ret;

	qp = dev->data->queue_pairs[qp_id];
	if (qp == NULL)
		return -EINVAL;

	ret = cn10k_ml_qp_destroy(dev, qp);
	if (ret) {
		plt_err("Could not destroy queue pair %d", qp_id);
		return ret;
	}

	dev->data->queue_pairs[qp_id] = NULL;

	return 0;
}

static int
ml_enqueue(struct rte_mldev *dev, struct cn10k_ml_qp *qp, struct rte_ml_op *op,
	   struct cn10k_ml_pending_queue *pend_q)
{
	struct cnxk_ml_job_compl *ml_job_compl;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_model *ml_model;
	struct cnxk_ml_dev *ml_dev;
	struct cnxk_ml_jd *jd;
	int ret = 0;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ml_model = ml_config->ml_models[op->model_id];

	ml_job_compl = NULL;
	ret = rte_mempool_get(ml_config->job_pool, (void **)(&ml_job_compl));
	if (ret != 0)
		return ret;

	/* Prepare JD */
	jd = &ml_job_compl->jd;
	cnxk_ml_prep_fp_job_descriptor(dev, ml_model, jd, ml_job_compl, op);

	ml_job_compl->job_result.status = ML_STATUS_FAILURE;
	ml_job_compl->job_result.user_ptr = op->user_ptr;

	plt_write64(ML_CN10K_POLL_JOB_START, &ml_job_compl->status_ptr);
	plt_wmb();

	ml_job_compl->start_cycle = plt_tsc_cycles();
	if (roc_ml_jcmdq_enqueue(&ml_dev->roc, jd)) {
		pend_q->req_queue[pend_q->enq_tail].op_handle = (uintptr_t)op;
		pend_q->req_queue[pend_q->enq_tail].job_handle =
			(uintptr_t)ml_job_compl;

		ML_MOD_INC(pend_q->enq_tail, qp->nb_desc);
		pend_q->pending_count += 1;
	} else {
		rte_mempool_put(ml_config->job_pool, ml_job_compl);
		ret = -1;
	}

	return ret;
}

uint16_t
cn10k_ml_enqueue_burst(struct rte_mldev *dev, uint16_t qp_id,
		       struct rte_ml_op **ops, uint16_t nb_ops)
{
	struct cn10k_ml_pending_queue *pend_q;
	struct cn10k_ml_qp *qp;
	struct rte_ml_op *op;
	uint16_t nb_allowed;
	uint16_t count;
	int ret;

	qp = dev->data->queue_pairs[qp_id];
	pend_q = &qp->pend_q;

	nb_allowed = qp->nb_desc - pend_q->pending_count;
	if (nb_ops > nb_allowed)
		nb_ops = nb_allowed;

	/*  Queue jobs */
	for (count = 0; count < nb_ops; count++) {
		op = ops[count];
		ret = ml_enqueue(dev, qp, op, pend_q);

		if (ret != 0)
			break;
	}

	return count;
}

static void
cnxk_ml_result_update(struct cnxk_ml_job_result *job_result,
		      struct rte_ml_op *op)
{
	op->result.success =
		(job_result->status == ML_STATUS_SUCCESS) ? true : false;
	op->result.error_code = job_result->error_code;
	op->user_ptr = job_result->user_ptr;
}

static int
ml_dequeue(struct rte_mldev *dev, struct rte_ml_op *op,
	   struct cn10k_ml_req *req)
{
	struct cnxk_ml_job_compl *ml_job_compl;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_dev *ml_dev;
	int ret = 0;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ml_job_compl = (struct cnxk_ml_job_compl *)(req->job_handle);

	if (ml_job_compl->job_result.status == ML_STATUS_SUCCESS) {
		ret = 0;
	} else if (ml_job_compl->job_result.status == ML_STATUS_FAILURE) {
		plt_err("Model run failed, model_id = %d", op->model_id);
		ret = -1;
	}

	cnxk_ml_result_update(&ml_job_compl->job_result, op);
	rte_mempool_put(ml_config->job_pool, ml_job_compl);

	return ret;
}

uint16_t
cn10k_ml_dequeue_burst(struct rte_mldev *dev, uint16_t qp_id,
		       struct rte_ml_op **ops, uint16_t nb_ops)
{
	struct cnxk_ml_job_compl *ml_job_compl;
	struct cn10k_ml_pending_queue *pend_q;
	struct cn10k_ml_req *req;
	struct cn10k_ml_qp *qp;
	uint16_t nb_completed;
	uint16_t nb_pending;

	qp = dev->data->queue_pairs[qp_id];
	pend_q = &qp->pend_q;
	nb_pending = pend_q->pending_count;

	if (nb_ops > nb_pending)
		nb_ops = nb_pending;

	for (nb_completed = 0; nb_completed < nb_ops; nb_completed++) {
		req = &pend_q->req_queue[pend_q->deq_head];
		ml_job_compl = (struct cnxk_ml_job_compl *)(req->job_handle);

		if (plt_read64(&ml_job_compl->status_ptr) !=
		    ML_CN10K_POLL_JOB_FINISH)
			break;

		ops[nb_completed] = (struct rte_ml_op *)(req->op_handle);
		ml_dequeue(dev, ops[nb_completed], req);

		ML_MOD_INC(pend_q->deq_head, qp->nb_desc);
		pend_q->pending_count -= 1;
	}

	return nb_completed;
}

int
cn10k_ml_inference_sync(struct rte_mldev *dev, struct rte_ml_op *op)
{
	struct cnxk_ml_job_compl *ml_job_compl;
	struct cnxk_ml_config *ml_config;
	struct cnxk_ml_model *ml_model;
	struct cnxk_ml_dev *ml_dev;
	struct cnxk_ml_jd *jd;
	bool timeout;
	int ret;

	ml_dev = dev->data->dev_private;
	ml_config = &ml_dev->ml_config;
	ml_model = ml_config->ml_models[op->model_id];

	ml_job_compl = NULL;
	ret = rte_mempool_get(ml_config->job_pool, (void **)(&ml_job_compl));
	if (ret != 0)
		return ret;

	/* Prepare JD */
	jd = &ml_job_compl->jd;
	cnxk_ml_prep_fp_job_descriptor(dev, ml_model, jd, ml_job_compl, op);

	ml_job_compl->job_result.status = ML_STATUS_FAILURE;
	ml_job_compl->job_result.user_ptr = op->user_ptr;

	plt_write64(ML_CN10K_POLL_JOB_START, &ml_job_compl->status_ptr);
	plt_wmb();

	timeout = true;
	ml_job_compl->start_cycle = plt_tsc_cycles();
	do {
		if (roc_ml_jcmdq_enqueue(&ml_dev->roc, jd)) {
			timeout = false;
			break;
		}
	} while (plt_tsc_cycles() - ml_job_compl->start_cycle <
		 ml_config->timeout.run);

	if (timeout) {
		ret = -EBUSY;
		goto error_enqueue;
	}

	timeout = true;
	ml_job_compl->start_cycle = plt_tsc_cycles();
	do {
		if (plt_read64(&ml_job_compl->status_ptr) ==
		    ML_CN10K_POLL_JOB_FINISH) {
			timeout = false;
			break;
		}
	} while (plt_tsc_cycles() - ml_job_compl->start_cycle <
		 ml_config->timeout.run);

	if (timeout) {
		ret = -ETIME;
	} else {
		if (ml_job_compl->job_result.status == ML_STATUS_SUCCESS) {
			timeout = false;
			ret = 0;
		} else if (ml_job_compl->job_result.status ==
			   ML_STATUS_FAILURE) {
			timeout = false;
			ret = -1;
		}
		cnxk_ml_result_update(&ml_job_compl->job_result, op);
	}

error_enqueue:
	rte_mempool_put(ml_config->job_pool, ml_job_compl);

	return ret;
}

struct rte_mldev_ops cn10k_ml_ops = {
	/* Device control ops */
	.dev_configure = cn10k_ml_dev_configure,
	.dev_close = cn10k_ml_dev_close,
	.dev_start = cn10k_ml_dev_start,
	.dev_stop = cn10k_ml_dev_stop,
	.dev_info_get = cn10k_ml_dev_info_get,

	/* Queue-pair handling ops */
	.queue_pair_setup = cn10k_ml_queue_pair_setup,
	.queue_pair_release = cn10k_ml_queue_pair_release,

	/* ML model handling ops */
	.ml_model_create = cn10k_ml_model_create,
	.ml_model_destroy = cn10k_ml_model_destroy,
	.ml_model_load = cn10k_ml_model_load,
	.ml_model_unload = cn10k_ml_model_unload,
	.ml_model_info_get = cn10k_ml_model_info_get,
};