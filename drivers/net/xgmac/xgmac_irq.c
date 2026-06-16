/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Marvell.
 */

#include <errno.h>
#include <unistd.h>

#include <bus_platform_driver.h>
#include <ethdev_driver.h>
#include <rte_bitops.h>
#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_interrupts.h>

#include "xgmac_dev.h"
#include "xgmac_ethdev.h"
#include "xgmac_regs.h"

/* macirq is always VFIO IRQ index 0 (the platform bus subscribes it as the
 * primary fd). Per-channel RX/TX lines, if the DT exposes them, live at
 * odd indices starting at 1: pair i is {1+2*i (RX), 2+2*i (TX)}. Only the
 * RX line is wired up; TX completions are polled.
 */
static void
xgmac_vfio_irq_layout(struct xgmac_dev *dev, uint32_t count)
{
	uint32_t pairs;
	uint32_t i;

	if (count <= 1)
		return;

	pairs = (count - 1) / 2;
	if (pairs == 0)
		return;

	if (pairs > dev->hw_feat.rx_q_cnt)
		pairs = dev->hw_feat.rx_q_cnt;

	dev->irq_multi = 1;
	dev->irq_n_rxq = pairs;

	for (i = 0; i < pairs; i++)
		dev->irq_rxq_vfio_idx[i] = 1 + i * 2;
}

static int
xgmac_vfio_irq_setup(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	const struct rte_platform_device *pdev = dev->pdev;

	if (pdev->dev_fd < 0 || pdev->intr_handle == NULL) {
		XGMAC_LOG(WARNING, "no VFIO intr_handle; interrupts disabled");
		return 0;
	}

	if (pdev->num_irqs == 0) {
		XGMAC_LOG(WARNING, "device exposes no VFIO IRQs; interrupts disabled");
		return 0;
	}

	dev->irq_event_fd = rte_intr_fd_get(pdev->intr_handle);
	if (dev->irq_event_fd < 0) {
		XGMAC_LOG(WARNING, "macirq not available; interrupts disabled");
		return 0;
	}

	eth_dev->intr_handle = pdev->intr_handle;

	XGMAC_LOG(INFO, "macirq fd=%d multi=%u total=%u", dev->irq_event_fd,
		  dev->irq_multi, pdev->num_irqs);
	return 0;
}

static void
xgmac_vfio_irq_teardown(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;

	dev->irq_event_fd = -1;
	eth_dev->intr_handle = NULL;
}

static void
xgmac_dma_intm_program(struct xgmac_dev *dev)
{
	uint32_t val = xgmac_rd(dev, XGMAC_DMA_MODE);
	uint32_t mode = dev->irq_multi ? XGMAC_DMA_MODE_INTM_RXTX_SPLIT
				       : XGMAC_DMA_MODE_INTM_COMBINED;

	val &= ~XGMAC_DMA_MODE_INTM;
	val |= XGMAC_FIELD_PREP(XGMAC_DMA_MODE_INTM, mode);
	xgmac_wr(dev, XGMAC_DMA_MODE, val);
}

int
xgmac_rxq_intr_setup(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	const struct rte_platform_device *pdev = dev->pdev;
	int n_rx_ok;
	uint16_t q;

	if (eth_dev->intr_handle == NULL || !eth_dev->data->dev_conf.intr_conf.rxq)
		return 0;

	xgmac_vfio_irq_layout(dev, pdev->num_irqs);
	if (!dev->irq_multi)
		return 0;

	if (rte_intr_vec_list_alloc(pdev->intr_handle, "xgmac-rxq", dev->irq_n_rxq) < 0) {
		XGMAC_LOG(WARNING, "vec list alloc failed; per-queue Rx intr disabled");
		dev->irq_multi = 0;
		dev->irq_n_rxq = 0;
		return 0;
	}

	n_rx_ok = rte_platform_intr_efd_enable_range(pdev, 1, 2, dev->irq_n_rxq);
	if (n_rx_ok <= 0) {
		XGMAC_LOG(WARNING, "per-channel intr setup failed; per-queue Rx intr disabled");
		rte_intr_vec_list_free(pdev->intr_handle);
		dev->irq_multi = 0;
		dev->irq_n_rxq = 0;
		return 0;
	}

	dev->irq_n_rxq = (uint16_t)n_rx_ok;
	/* Map each Rx queue to its interrupt vector so
	 * rte_eth_dev_rx_intr_ctl_q() can resolve the per-queue eventfd.
	 */
	for (q = 0; q < dev->irq_n_rxq; q++)
		rte_intr_vec_list_index_set(pdev->intr_handle, q,
					    RTE_INTR_VEC_RXTX_OFFSET + q);
	/* Route per-channel events if per-queue Rx interrupts are armed. */
	xgmac_dma_intm_program(dev);

	return 0;
}

void
xgmac_rxq_intr_teardown(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	const struct rte_platform_device *pdev = dev->pdev;

	if (eth_dev->intr_handle != NULL && dev->irq_multi) {
		rte_platform_intr_efd_disable_range(pdev, 1, 2, dev->irq_n_rxq);
		rte_intr_vec_list_free(pdev->intr_handle);
	}
	dev->irq_multi = 0;
	dev->irq_n_rxq = 0;
}

static void
xgmac_dev_interrupt_handler(void *param)
{
	struct rte_eth_dev *eth_dev = param;
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint32_t st, clear;
	uint16_t nb_ch, q;
	uint64_t evt_val;
	ssize_t r;

	/* Drain the eventfd so EAL doesn't re-fire us (handle is VDEV-typed). */
	if (dev->irq_event_fd >= 0) {
		do {
			r = read(dev->irq_event_fd, &evt_val, sizeof(evt_val));
		} while (r == sizeof(evt_val));
	}

	nb_ch = RTE_MAX(eth_dev->data->nb_rx_queues, eth_dev->data->nb_tx_queues);
	nb_ch = RTE_MAX(nb_ch, 1);

	for (q = 0; q < nb_ch; q++) {
		st = xgmac_rd(dev, XGMAC_DMA_CH_STATUS(q));

		if (!st)
			continue;

		if (st & XGMAC_AIS) {
			XGMAC_LOG(WARNING, "q%u abnormal irq: DMA_CH_STATUS=0x%08x", q, st);
			if (st & XGMAC_FBE) {
				XGMAC_LOG(ERR, "q%u fatal bus error", q);
				rte_eth_dev_callback_process(eth_dev,
					RTE_ETH_EVENT_INTR_RESET, NULL);
			}
			if (st & XGMAC_CDE)
				XGMAC_LOG(ERR,
					  "q%u TSO context-descriptor error "
					  "(check TSO header/MSS/payload limits)",
					  q);
		}

		/* In per-channel mode, RI/TI are owned by per-channel lines —
		 * don't steal them or the user-side rx-intr path will miss wakeups.
		 */
		clear = st;
		if (dev->irq_multi)
			clear &= ~(XGMAC_RI | XGMAC_TI);
		if (clear)
			xgmac_wr(dev, XGMAC_DMA_CH_STATUS(q), clear);
	}
	rte_platform_irq_unmask(dev->pdev, 0);
}

int
xgmac_rx_queue_intr_enable(struct rte_eth_dev *eth_dev, uint16_t qid)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint32_t v;

	if (qid >= eth_dev->data->nb_rx_queues)
		return -EINVAL;

	if (!dev->irq_multi || qid >= dev->irq_n_rxq) {
		XGMAC_LOG(WARNING, "rx_queue_intr_enable q%u: per-channel IRQ not available", qid);
		return -ENOTSUP;
	}

	xgmac_wr(dev, XGMAC_DMA_CH_STATUS(qid), XGMAC_RI);

	v = xgmac_rd(dev, XGMAC_DMA_CH_INT_EN(qid));
	v |= XGMAC_RIE;
	xgmac_wr(dev, XGMAC_DMA_CH_INT_EN(qid), v);

	/* Re-arm the queue line; bus no-ops this if it isn't automasked. */
	return rte_platform_irq_unmask(dev->pdev, dev->irq_rxq_vfio_idx[qid]);
}

int
xgmac_rx_queue_intr_disable(struct rte_eth_dev *eth_dev, uint16_t qid)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	uint32_t v;

	if (qid >= eth_dev->data->nb_rx_queues)
		return -EINVAL;

	if (!dev->irq_multi || qid >= dev->irq_n_rxq)
		return -ENOTSUP;

	v = xgmac_rd(dev, XGMAC_DMA_CH_INT_EN(qid));
	v &= ~XGMAC_RIE;
	xgmac_wr(dev, XGMAC_DMA_CH_INT_EN(qid), v);
	return 0;
}

int
xgmac_intr_register(struct rte_eth_dev *eth_dev)
{
	struct xgmac_dev *dev = eth_dev->data->dev_private;
	int ret;

	dev->irq_event_fd = -1;

	ret = xgmac_vfio_irq_setup(eth_dev);
	if (ret < 0)
		return ret;

	if (eth_dev->intr_handle == NULL) {
		XGMAC_LOG(WARNING,
			  "no VFIO IRQ subscribed; DMA-error events not reported");
		return 0;
	}

	ret = rte_intr_callback_register(eth_dev->intr_handle,
					 xgmac_dev_interrupt_handler, eth_dev);
	if (ret < 0) {
		XGMAC_LOG(ERR, "failed to register interrupt callback");
		xgmac_vfio_irq_teardown(eth_dev);
		return ret;
	}

	return 0;
}

void
xgmac_intr_unregister(struct rte_eth_dev *eth_dev)
{
	/* Drop any per-queue lines still armed (e.g. close without stop). */
	xgmac_rxq_intr_teardown(eth_dev);

	if (eth_dev->intr_handle)
		rte_intr_callback_unregister(eth_dev->intr_handle, xgmac_dev_interrupt_handler,
					     eth_dev);
	xgmac_vfio_irq_teardown(eth_dev);
}
