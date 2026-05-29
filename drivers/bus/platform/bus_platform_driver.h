/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2023 Marvell.
 */

#ifndef BUS_PLATFORM_DRIVER_H
#define BUS_PLATFORM_DRIVER_H

/**
 * @file
 * Platform bus interface.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dev_driver.h>
#include <rte_bitops.h>
#include <rte_common.h>
#include <rte_compat.h>
#include <rte_dev.h>
#include <rte_os.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct rte_intr_handle;
struct rte_platform_bus;
struct rte_platform_device;
struct rte_platform_driver;

/**
 * Initialization function for the driver called during platform device probing.
 *
 * @param pdev
 *   Pointer to the platform device.
 * @return
 *   0 on success, negative value otherwise.
 */
typedef int (rte_platform_probe_t)(struct rte_platform_device *pdev);

/**
 * Removal function for the driver called during platform device removal.
 *
 * @param pdev
 *   Pointer to the platform device.
 * @return
 *   0 on success, negative value otherwise.
 */
typedef int (rte_platform_remove_t)(struct rte_platform_device *pdev);

/**
 * Driver specific DMA mapping.
 *
 * @param pdev
 *   Pointer to the platform device.
 * @param addr
 *   Starting virtual address of memory to be mapped.
 * @param iova
 *   Starting IOVA address of memory to be mapped.
 * @param len
 *   Length of memory segment being mapped.
 * @return
 *   - 0 on success, negative value and rte_errno is set otherwise.
 */
typedef int (rte_platform_dma_map_t)(struct rte_platform_device *pdev, void *addr, uint64_t iova,
				     size_t len);

/**
 * Driver specific DMA unmapping.
 *
 * @param pdev
 *   Pointer to the platform device.
 * @param addr
 *   Starting virtual address of memory to be mapped.
 * @param iova
 *   Starting IOVA address of memory to be mapped.
 * @param len
 *   Length of memory segment being mapped.
 * @return
 *   - 0 on success, negative value and rte_errno is set otherwise.
 */
typedef int (rte_platform_dma_unmap_t)(struct rte_platform_device *pdev, void *addr, uint64_t iova,
				       size_t len);

/**
 * A structure describing a platform device resource.
 */
struct rte_platform_resource {
	char *name; /**< Resource name specified via reg-names prop in device-tree */
	struct rte_mem_resource mem; /**< Memory resource */
};

/**
 * A structure describing a platform device.
 */
struct rte_platform_device {
	RTE_TAILQ_ENTRY(rte_platform_device) next; /**< Next attached platform device */
	struct rte_device device; /**< Core device */
	struct rte_platform_driver *driver; /**< Matching device driver */
	char name[RTE_DEV_NAME_MAX_LEN]; /**< Device name */
	unsigned int num_resource; /**< Number of device resources */
	struct rte_platform_resource *resource; /**< Device resources */
	int dev_fd; /**< VFIO device fd */
	uint32_t num_irqs; /**< Number of VFIO IRQ indices */
	struct rte_intr_handle *intr_handle; /**< Interrupt handle */
};

/**
 * A structure describing a platform device driver.
 */
struct rte_platform_driver {
	RTE_TAILQ_ENTRY(rte_platform_driver) next; /**< Next available platform driver */
	struct rte_driver driver; /**< Core driver */
	rte_platform_probe_t *probe;  /**< Device probe function */
	rte_platform_remove_t *remove; /**< Device remove function */
	rte_platform_dma_map_t *dma_map; /**< Device DMA map function */
	rte_platform_dma_unmap_t *dma_unmap; /**< Device DMA unmap function */
	uint32_t drv_flags; /**< Driver flags RTE_PLATFORM_DRV_* */
};

/** Device driver needs IOVA as VA and cannot work with IOVA as PA */
#define RTE_PLATFORM_DRV_NEED_IOVA_AS_VA 0x0001

/**
 * @internal
 * Helper macros used to convert core device to platform device.
 */
#define RTE_DEV_TO_PLATFORM_DEV(ptr) \
	container_of(ptr, struct rte_platform_device, device)

#define RTE_DEV_TO_PLATFORM_DEV_CONST(ptr) \
	container_of(ptr, const struct rte_platform_device, device)

/** Helper for platform driver registration. */
#define RTE_PMD_REGISTER_PLATFORM(nm, platform_drv) \
static const char *pdrvinit_ ## nm ## _alias; \
RTE_INIT(pdrvinitfn_ ##nm) \
{ \
	(platform_drv).driver.name = RTE_STR(nm); \
	(platform_drv).driver.alias = pdrvinit_ ## nm ## _alias; \
	rte_platform_register(&(platform_drv)); \
} \
RTE_PMD_EXPORT_NAME(nm)

/** Helper for setting platform driver alias. */
#define RTE_PMD_REGISTER_ALIAS(nm, alias) \
static const char *pdrvinit_ ## nm ## _alias = RTE_STR(alias)

/**
 * Register a platform device driver.
 *
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice.
 *
 * @param pdrv
 *   A pointer to a rte_platform_driver structure describing driver to be registered.
 */
__rte_internal
void rte_platform_register(struct rte_platform_driver *pdrv);

/**
 * Unregister a platform device driver.
 *
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice.
 *
 * @param pdrv
 *   A pointer to a rte_platform_driver structure describing driver to be unregistered.
 */
__rte_internal
void rte_platform_unregister(struct rte_platform_driver *pdrv);

/**
 * Unmask (re-arm) a VFIO IRQ index. No-op for indices the kernel does not
 * mask, so it may be called unconditionally after an event is handled.
 *
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice.
 *
 * @param pdev
 *   Platform device.
 * @param idx
 *   VFIO IRQ index to unmask.
 * @return
 *   0 on success, negative errno on failure.
 */
__rte_internal
int rte_platform_irq_unmask(const struct rte_platform_device *pdev, uint32_t idx);

/**
 * Explicitly mask a VFIO IRQ index.
 *
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice.
 *
 * @param pdev
 *   Platform device.
 * @param idx
 *   VFIO IRQ index to mask.
 * @return
 *   0 on success, negative errno on failure.
 */
__rte_internal
int rte_platform_irq_mask(const struct rte_platform_device *pdev, uint32_t idx);

/**
 * Subscribe a VFIO IRQ index as an eventfd in intr_handle->efds[@p slot].
 *
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice.
 *
 * @param pdev
 *   Platform device.
 * @param slot
 *   Index into intr_handle->efds[]; a driver-chosen logical line number.
 * @param vfio_idx
 *   VFIO IRQ index to attach a fresh eventfd to.
 * @return
 *   0 on success, negative errno on failure.
 */
__rte_internal
int rte_platform_intr_efd_enable(const struct rte_platform_device *pdev,
				 uint16_t slot, uint32_t vfio_idx);

/**
 * Tear down an eventfd subscription installed by
 * rte_platform_intr_efd_enable().
 *
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice.
 *
 * @param pdev
 *   Platform device.
 * @param slot
 *   Slot in intr_handle->efds[] to release.
 * @param vfio_idx
 *   VFIO IRQ index originally passed at enable time.
 */
__rte_internal
void rte_platform_intr_efd_disable(const struct rte_platform_device *pdev,
				   uint16_t slot, uint32_t vfio_idx);

/**
 * Bulk variant of rte_platform_intr_efd_enable() for VFIO IRQ indices that
 * form an arithmetic sequence (start, start + stride, ...). Stops at the
 * first failure, keeping previously subscribed slots intact.
 *
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice.
 *
 * @param pdev
 *   Platform device.
 * @param start_vfio_idx
 *   VFIO IRQ index attached to slot 0.
 * @param stride
 *   Distance between consecutive VFIO IRQ indices.
 * @param n
 *   Number of slots to subscribe; fills slots 0..n-1.
 * @return
 *   Number of slots successfully subscribed (0..n).
 */
__rte_internal
int rte_platform_intr_efd_enable_range(const struct rte_platform_device *pdev,
				       uint32_t start_vfio_idx, uint32_t stride,
				       uint16_t n);

/**
 * Bulk variant of rte_platform_intr_efd_disable() that mirrors
 * rte_platform_intr_efd_enable_range(): tears down slots 0..n-1 whose
 * VFIO IRQ indices follow the same (start, stride) arithmetic.
 *
 * @warning
 * @b EXPERIMENTAL: this API may change without prior notice.
 *
 * @param pdev
 *   Platform device.
 * @param start_vfio_idx
 *   VFIO IRQ index originally attached to slot 0.
 * @param stride
 *   Same stride passed to rte_platform_intr_efd_enable_range().
 * @param n
 *   Number of slots to release.
 */
__rte_internal
void rte_platform_intr_efd_disable_range(const struct rte_platform_device *pdev,
					 uint32_t start_vfio_idx, uint32_t stride,
					 uint16_t n);

#ifdef __cplusplus
}
#endif

#endif /* BUS_PLATFORM_DRIVER_H */
