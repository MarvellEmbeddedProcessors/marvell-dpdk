/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2025 Marvell.
 */
#include <bus_pci_driver.h>
#include <dev_driver.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_pci.h>
#include <rte_rawdev.h>
#include <rte_rawdev_pmd.h>

#include "cnxk_emdev.h"
#include "cnxk_emdev_virtio.h"
#include "cnxk_emdev_vnet.h"
#include "spec/virtio.h"
#include <roc_api.h>

#define BIT_MASK32		   (0xFFFFFFFFU)
#define VIRTIO_INVALID_QUEUE_INDEX 0xFFFF
#define VIRTIO_DESC_SZ		   16
#define VIRTIO_DFLT_QUEUE_SZ	   4096

struct cnxk_emdev_virtio_cbs emdev_virtio_cbs[EMDEV_TYPE_MAX];

static int
cnxk_emdev_virtio_pfvf_init(struct cnxk_emdev *dev, uint16_t nb_pfvfs,
			    struct rte_pmd_cnxk_emdev_conf *conf)
{
	struct cnxk_emdev_virtio_pfvf *pfvf, *pfvfs;
	uint16_t max_queues;
	int i, rc;

	max_queues = dev->roc_emdev.nb_outb_qs;

	pfvfs = plt_zmalloc(nb_pfvfs * sizeof(struct cnxk_emdev_virtio_pfvf), 0);
	if (!pfvfs) {
		plt_err("Couldn't allocate memory for emdev VFs");
		return -ENOMEM;
	}
	for (i = 0; i < (int)nb_pfvfs; i++) {
		pfvf = &pfvfs[i];
		pfvf->dev = dev;
		pfvf->vf_id = i;
		pfvf->max_queues = max_queues;

		/* Allocate per virtio queue control path/fast path */
		pfvf->queue_conf =
			plt_zmalloc(max_queues * sizeof(struct cnxk_emdev_virtio_queue_conf), 0);
		if (!pfvf->queue_conf) {
			plt_err("Failed to allocate memory for queue config");
			i--;
			rc = -ENOMEM;
			goto exit;
		}
		for (int j = 0; j < (int)max_queues; j++) {
			pfvf->queue_conf[j].queue_select = VIRTIO_INVALID_QUEUE_INDEX;
			pfvf->queue_conf[j].queue_size = VIRTIO_DFLT_QUEUE_SZ;
		}
		pfvf->vnet_qs = plt_zmalloc(max_queues * sizeof(struct cnxk_emdev_vnet_queue), 64);
		if (!pfvf->vnet_qs) {
			plt_err("Failed to allocate memory for vnet queue config");
			plt_free(pfvf->queue_conf);
			i--;
			rc = -ENOMEM;
			goto exit;
		}
		/* Set default device feature bits */
		pfvf->dev_feature_bits =
			RTE_BIT64(VIRTIO_F_RING_PACKED) | RTE_BIT64(VIRTIO_F_VERSION_1) |
			RTE_BIT64(VIRTIO_F_ANY_LAYOUT) | RTE_BIT64(VIRTIO_F_IN_ORDER) |
			RTE_BIT64(VIRTIO_F_ORDER_PLATFORM) | RTE_BIT64(VIRTIO_F_SR_IOV) |
			RTE_BIT64(VIRTIO_F_IOMMU_PLATFORM) | RTE_BIT64(VIRTIO_F_NOTIFICATION_DATA);
		pfvf->emdev_type = conf->emdev_type;
		pfvf->status_cb = conf->status_cb;
		switch (conf->emdev_type) {
		case EMDEV_TYPE_VIRTIO_NET:
			cnxk_emdev_vnet_init(pfvf, &conf->vnet_conf[i]);
			break;
		default:
			break;
		}
	}
	dev->pfvf = pfvfs;

	return 0;
exit:
	for (; i >= 0; i--) {
		plt_free(pfvfs[i].vnet_qs);
		plt_free(pfvfs[i].queue_conf);
	}
	plt_free(pfvfs);

	return rc;
}

static void
cnxk_emdev_virtio_pfvf_fini(struct cnxk_emdev *dev, uint16_t nb_pfvfs)
{
	struct cnxk_emdev_virtio_pfvf *pfvf, *pfvfs = dev->pfvf;
	int i;

	for (i = 0; i < (int)nb_pfvfs; i++) {
		pfvf = &pfvfs[i];
		plt_free(pfvf->queue_conf);
		plt_free(pfvf->vnet_qs);
	}
	plt_free(dev->pfvf);
}

int
cnxk_emdev_virtio_setup(struct cnxk_emdev *dev, struct rte_pmd_cnxk_emdev_conf *conf)
{
	struct roc_emdev *roc_emdev = &dev->roc_emdev;
	int rc;

	rc = cnxk_emdev_virtio_pfvf_init(dev, roc_emdev->nb_epfvfs, conf);
	if (rc)
		return rc;

	return 0;
}

void
cnxk_emdev_virtio_close(struct cnxk_emdev *dev)
{
	struct roc_emdev *roc_emdev = &dev->roc_emdev;

	cnxk_emdev_virtio_pfvf_fini(dev, roc_emdev->nb_epfvfs);
}
