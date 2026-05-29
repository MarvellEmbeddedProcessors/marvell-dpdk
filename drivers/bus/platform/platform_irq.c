/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2026 Marvell.
 */

/*
 * VFIO IRQ support for the platform bus.
 *
 * This file mirrors the PCI bus model: the platform bus pre-discovers
 * VFIO IRQ information at probe time, pre-allocates an rte_intr_handle,
 * and subscribes VFIO IRQ index 0 (the primary, equivalent to PCI's
 * MSI-X vector 0 or DT's "macirq") into intr_handle->fd. PMDs just
 * consume pdev->intr_handle and, if they expose per-queue lines, loop
 * over rte_platform_intr_efd_enable() to wire each additional eventfd.
 */

#include <uapi/linux/vfio.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <bus_platform_driver.h>
#include <eal_export.h>
#include <rte_interrupts.h>

#include "private.h"

static int
platform_irq_set(int dev_fd, uint32_t idx, uint32_t flags, int eventfd_fd)
{
	char buf[sizeof(struct vfio_irq_set) + sizeof(int)] = { 0 };
	struct vfio_irq_set *s = (struct vfio_irq_set *)buf;

	s->argsz = sizeof(struct vfio_irq_set);
	s->flags = flags;
	s->index = idx;
	s->start = 0;
	if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		s->argsz = sizeof(buf);
		s->count = 1;
		memcpy(&s->data, &eventfd_fd, sizeof(int));
	} else if (flags & (VFIO_IRQ_SET_ACTION_UNMASK | VFIO_IRQ_SET_ACTION_MASK)) {
		s->count = 1;
	} else {
		s->count = 0;
	}

	if (ioctl(dev_fd, VFIO_DEVICE_SET_IRQS, s) != 0)
		return -errno;
	return 0;
}

static int
platform_irq_subscribe(const struct rte_platform_device *pdev, uint32_t idx,
		       bool initial_unmask)
{
	int evt_fd, ret;

	evt_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (evt_fd < 0)
		return -errno;

	ret = platform_irq_set(pdev->dev_fd, idx,
			       VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER, evt_fd);
	if (ret != 0) {
		close(evt_fd);
		return ret;
	}

	if (initial_unmask) {
		ret = platform_irq_set(pdev->dev_fd, idx,
				       VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK, -1);
		if (ret != 0)
			PLATFORM_LOG_LINE(WARNING, "initial unmask failed for VFIO idx=%u: %s",
					  idx, strerror(-ret));
	}

	return evt_fd;
}

static void
platform_irq_unsubscribe(const struct rte_platform_device *pdev, uint32_t idx,
			 int eventfd_fd)
{
	platform_irq_set(pdev->dev_fd, idx, VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
			 -1);
	if (eventfd_fd >= 0)
		close(eventfd_fd);
}

/* Bus-private capability flags for a VFIO index (0 if out of range). */
static uint32_t
platform_irq_flags(const struct rte_platform_device *pdev, uint32_t idx)
{
	const struct platform_device *pdata = platform_dev_const(pdev);

	if (pdata->irq_info == NULL || idx >= pdev->num_irqs)
		return 0;
	return pdata->irq_info[idx].flags;
}

int
platform_discover_irqs(struct rte_platform_device *pdev)
{
	struct vfio_device_info dev_info = { .argsz = sizeof(dev_info) };
	struct platform_irq_info *info;
	uint32_t i;

	if (ioctl(pdev->dev_fd, VFIO_DEVICE_GET_INFO, &dev_info) != 0)
		return -errno;

	pdev->num_irqs = dev_info.num_irqs;
	platform_dev(pdev)->irq_info = NULL;
	if (dev_info.num_irqs == 0)
		return 0;

	info = calloc(dev_info.num_irqs, sizeof(*info));
	if (info == NULL)
		return -ENOMEM;

	for (i = 0; i < dev_info.num_irqs; i++) {
		struct vfio_irq_info irq = { .argsz = sizeof(irq), .index = i };

		if (ioctl(pdev->dev_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq) != 0) {
			free(info);
			pdev->num_irqs = 0;
			return -errno;
		}

		info[i].count = irq.count;
		if (irq.flags & VFIO_IRQ_INFO_EVENTFD)
			info[i].flags |= PLATFORM_IRQ_F_EVENTFD;
		if (irq.flags & VFIO_IRQ_INFO_AUTOMASKED)
			info[i].flags |= PLATFORM_IRQ_F_AUTOMASKED;
		if (irq.flags & VFIO_IRQ_INFO_MASKABLE)
			info[i].flags |= PLATFORM_IRQ_F_MASKABLE;
	}

	platform_dev(pdev)->irq_info = info;
	return 0;
}

void
platform_release_irqs(struct rte_platform_device *pdev)
{
	free(platform_dev(pdev)->irq_info);
	platform_dev(pdev)->irq_info = NULL;
	pdev->num_irqs = 0;
}

int
platform_setup_intr_handle(struct rte_platform_device *pdev)
{
	const struct platform_irq_info *info;
	bool automasked;
	int primary_fd;

	pdev->intr_handle = rte_intr_instance_alloc(RTE_INTR_INSTANCE_F_PRIVATE);
	if (pdev->intr_handle == NULL)
		return -ENOMEM;

	if (rte_intr_dev_fd_set(pdev->intr_handle, pdev->dev_fd) != 0 ||
	    rte_intr_type_set(pdev->intr_handle, RTE_INTR_HANDLE_VDEV) != 0 ||
	    rte_intr_efd_counter_size_set(pdev->intr_handle, sizeof(uint64_t)) != 0 ||
	    rte_intr_fd_set(pdev->intr_handle, -1) != 0) {
		rte_intr_instance_free(pdev->intr_handle);
		pdev->intr_handle = NULL;
		return -EINVAL;
	}

	/*
	 * Subscribe VFIO IRQ index 0 (the primary). By platform-bus
	 * convention this is the device-wide event line.
	 * Devices that do not expose an eventfd-deliverable index 0 just
	 * keep the pre-allocated handle with fd = -1; PMDs detect this
	 * via rte_intr_fd_get() < 0.
	 */
	if (pdev->num_irqs == 0 || platform_dev(pdev)->irq_info == NULL)
		return 0;

	info = &platform_dev(pdev)->irq_info[0];
	if (info->count == 0 || !(info->flags & PLATFORM_IRQ_F_EVENTFD)) {
		PLATFORM_LOG_LINE(INFO, "%s: primary VFIO idx=0 not eventfd-deliverable",
				  pdev->name);
		return 0;
	}

	automasked = !!(info->flags & PLATFORM_IRQ_F_AUTOMASKED);
	primary_fd = platform_irq_subscribe(pdev, 0, automasked);
	if (primary_fd < 0) {
		PLATFORM_LOG_LINE(WARNING, "%s: primary VFIO idx=0 subscribe failed: %s",
				  pdev->name, strerror(-primary_fd));
		return 0;
	}

	if (rte_intr_fd_set(pdev->intr_handle, primary_fd) != 0) {
		platform_irq_unsubscribe(pdev, 0, primary_fd);
		return -EINVAL;
	}

	return 0;
}

void
platform_teardown_intr_handle(struct rte_platform_device *pdev)
{
	int fd;

	if (pdev->intr_handle == NULL)
		return;

	fd = rte_intr_fd_get(pdev->intr_handle);
	if (fd >= 0)
		platform_irq_unsubscribe(pdev, 0, fd);

	rte_intr_free_epoll_fd(pdev->intr_handle);
	rte_intr_vec_list_free(pdev->intr_handle);
	rte_intr_nb_efd_set(pdev->intr_handle, 0);
	rte_intr_max_intr_set(pdev->intr_handle, 0);
	rte_intr_instance_free(pdev->intr_handle);
	pdev->intr_handle = NULL;
}

RTE_EXPORT_INTERNAL_SYMBOL(rte_platform_irq_unmask)
int
rte_platform_irq_unmask(const struct rte_platform_device *pdev, uint32_t idx)
{
	/* Nothing to do for indices the kernel does not mask. */
	if (!(platform_irq_flags(pdev, idx) &
	      (PLATFORM_IRQ_F_AUTOMASKED | PLATFORM_IRQ_F_MASKABLE)))
		return 0;

	return platform_irq_set(pdev->dev_fd, idx,
				VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK, -1);
}

RTE_EXPORT_INTERNAL_SYMBOL(rte_platform_irq_mask)
int
rte_platform_irq_mask(const struct rte_platform_device *pdev, uint32_t idx)
{
	if (!(platform_irq_flags(pdev, idx) & PLATFORM_IRQ_F_MASKABLE))
		return 0;

	return platform_irq_set(pdev->dev_fd, idx,
				VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_MASK, -1);
}

RTE_EXPORT_INTERNAL_SYMBOL(rte_platform_intr_efd_enable)
int
rte_platform_intr_efd_enable(const struct rte_platform_device *pdev, uint16_t slot,
			     uint32_t vfio_idx)
{
	struct rte_intr_handle *h = pdev->intr_handle;
	int fd, nb, max_intr;
	bool automasked;

	/* The bus knows whether this index needs an initial unmask. */
	automasked = !!(platform_irq_flags(pdev, vfio_idx) & PLATFORM_IRQ_F_AUTOMASKED);
	fd = platform_irq_subscribe(pdev, vfio_idx, automasked);
	if (fd < 0)
		return fd;

	if (rte_intr_efds_index_set(h, slot, fd) != 0) {
		platform_irq_unsubscribe(pdev, vfio_idx, fd);
		return -EINVAL;
	}

	/* Grow nb_efd / max_intr to cover this slot. */
	nb = rte_intr_nb_efd_get(h);
	if (slot + 1 > nb)
		rte_intr_nb_efd_set(h, slot + 1);
	max_intr = rte_intr_max_intr_get(h);
	if (slot + 1 > max_intr)
		rte_intr_max_intr_set(h, slot + 1);

	return 0;
}

RTE_EXPORT_INTERNAL_SYMBOL(rte_platform_intr_efd_disable)
void
rte_platform_intr_efd_disable(const struct rte_platform_device *pdev, uint16_t slot,
			      uint32_t vfio_idx)
{
	struct rte_intr_handle *h = pdev->intr_handle;
	int fd;

	fd = rte_intr_efds_index_get(h, slot);
	if (fd < 0)
		return;

	platform_irq_unsubscribe(pdev, vfio_idx, fd);
	rte_intr_efds_index_set(h, slot, -1);
}

RTE_EXPORT_INTERNAL_SYMBOL(rte_platform_intr_efd_enable_range)
int
rte_platform_intr_efd_enable_range(const struct rte_platform_device *pdev, uint32_t start_vfio_idx,
				   uint32_t stride, uint16_t n)
{
	uint16_t i;

	for (i = 0; i < n; i++) {
		uint32_t vfio_idx = start_vfio_idx + (uint32_t)i * stride;
		int ret = rte_platform_intr_efd_enable(pdev, i, vfio_idx);

		if (ret < 0) {
			PLATFORM_LOG_LINE(WARNING, "slot %u VFIO idx=%u subscribe failed: %s",
					  i, vfio_idx, strerror(-ret));
			return i;
		}
	}
	return n;
}

RTE_EXPORT_INTERNAL_SYMBOL(rte_platform_intr_efd_disable_range)
void
rte_platform_intr_efd_disable_range(const struct rte_platform_device *pdev, uint32_t start_vfio_idx,
				    uint32_t stride, uint16_t n)
{
	uint16_t i;

	for (i = 0; i < n; i++) {
		uint32_t vfio_idx = start_vfio_idx + (uint32_t)i * stride;

		rte_platform_intr_efd_disable(pdev, i, vfio_idx);
	}
}
