/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2023 Marvell.
 */

#ifndef PLATFORM_PRIVATE_H
#define PLATFORM_PRIVATE_H

#include <bus_driver.h>
#include <rte_bitops.h>
#include <rte_bus.h>
#include <rte_common.h>
#include <rte_dev.h>
#include <rte_log.h>
#include <rte_os.h>

#include "bus_platform_driver.h"

extern struct rte_platform_bus platform_bus;

/* Platform bus iterators. */
#define FOREACH_DEVICE_ON_PLATFORM_BUS(p) \
	RTE_TAILQ_FOREACH(p, &(platform_bus.device_list), next)

#define FOREACH_DRIVER_ON_PLATFORM_BUS(p) \
	RTE_TAILQ_FOREACH(p, &(platform_bus.driver_list), next)

/*
 * Structure describing platform bus.
 */
struct rte_platform_bus {
	struct rte_bus bus; /* Core bus */
	RTE_TAILQ_HEAD(, rte_platform_device) device_list; /* List of bus devices */
	RTE_TAILQ_HEAD(, rte_platform_driver) driver_list; /* List of bus drivers */
};

extern int platform_bus_logtype;
#define RTE_LOGTYPE_PLATFORM_BUS platform_bus_logtype
#define PLATFORM_LOG_LINE(level, ...) \
	RTE_LOG_LINE(level, PLATFORM_BUS, __VA_ARGS__)

#define PLATFORM_IRQ_F_EVENTFD     RTE_BIT32(0) /* deliverable via eventfd */
#define PLATFORM_IRQ_F_AUTOMASKED  RTE_BIT32(1) /* kernel masks on delivery */
#define PLATFORM_IRQ_F_MASKABLE    RTE_BIT32(2) /* supports mask/unmask */

/*
 * Bus-private per-index VFIO IRQ info, cached at probe time.
 */
struct platform_irq_info {
	uint32_t count;  /* number of sub-vectors at this index */
	uint32_t flags;  /* bitmask of PLATFORM_IRQ_F_* */
};

/*
 * Bus-private device wrapper. The public rte_platform_device MUST be the
 * first member so the public pointer handed to PMDs and the bus-private
 * state share an address (recoverable via platform_dev()/_const()).
 */
struct platform_device {
	struct rte_platform_device pdev; /* public part, must be first */
	struct platform_irq_info *irq_info; /* per-index info, num_irqs long */
};

#define platform_dev(p) \
	container_of(p, struct platform_device, pdev)
#define platform_dev_const(p) \
	container_of(p, const struct platform_device, pdev)

/*
 * Iterate registered platform devices and find one that matches provided string.
 */
void *
platform_bus_dev_iterate(const void *start, const char *str,
			 const struct rte_dev_iterator *it __rte_unused);

/*
 * Discover VFIO IRQ indices for a platform device and populate
 * pdev->num_irqs (public) and the bus-private irq_info[]. Called once at
 * probe time after dev_fd is open.
 */
int platform_discover_irqs(struct rte_platform_device *pdev);

/*
 * Free the bus-private irq_info[] and reset pdev->num_irqs.
 */
void platform_release_irqs(struct rte_platform_device *pdev);

/*
 * Allocate pdev->intr_handle, pre-seed it (dev_fd, type=VDEV,
 * efd_counter_size=8), and subscribe VFIO IRQ index 0 (the primary) into
 * intr_handle->fd. Safe to call even when the device exposes no IRQs:
 * the intr_handle is still allocated but its primary fd stays -1.
 */
int platform_setup_intr_handle(struct rte_platform_device *pdev);

/*
 * Unsubscribe VFIO IRQ index 0 and free pdev->intr_handle.
 */
void platform_teardown_intr_handle(struct rte_platform_device *pdev);

#endif /* PLATFORM_PRIVATE_H */
