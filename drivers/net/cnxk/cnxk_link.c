/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#include "cnxk_ethdev.h"

/* For each requested speed, 'adv' is the link mode advertised to the firmware
 * and 'cgx_mode' is the bit checked against the firmware supported modes bitmap.
 * A speed may have several variants ordered as CR, KR and optical, and the first
 * variant that the firmware reports as supported is the one that gets advertised.
 */
static const struct {
	uint32_t rte_speed;
	uint64_t adv;
	uint8_t cgx_mode;
} nix_link_modes[] = {
	{RTE_ETH_LINK_SPEED_10M, ROC_NIX_LINK_MODE_10BASET_FD, ETH_MODE_SGMII_10M_BIT},
	{RTE_ETH_LINK_SPEED_100M, ROC_NIX_LINK_MODE_100BASET_FD, ETH_MODE_SGMII_100M_BIT},
	{RTE_ETH_LINK_SPEED_1G, ROC_NIX_LINK_MODE_1000BASEX_FD, CGX_MODE_1000_BASEX},
	{RTE_ETH_LINK_SPEED_1G, ROC_NIX_LINK_MODE_1000BASEKX_FD, CGX_MODE_SFI_1G_BIT},
	{RTE_ETH_LINK_SPEED_1G, ROC_NIX_LINK_MODE_1000BASET_FD, CGX_MODE_SGMII},
	{RTE_ETH_LINK_SPEED_2_5G, ROC_NIX_LINK_MODE_2500BASEX_FD, ETH_MODE_2500_BASEX_BIT},
	{RTE_ETH_LINK_SPEED_5G, ROC_NIX_LINK_MODE_5000BASET_FD, ETH_MODE_5000_BASEX_BIT},
	{RTE_ETH_LINK_SPEED_10G, ROC_NIX_LINK_MODE_10000BASESR_FD, CGX_MODE_10G_C2C},
	{RTE_ETH_LINK_SPEED_10G, ROC_NIX_LINK_MODE_10000BASELR_FD, CGX_MODE_10G_C2M},
	{RTE_ETH_LINK_SPEED_10G, ROC_NIX_LINK_MODE_10000BASEKR_FD, CGX_MODE_10G_KR},
	{RTE_ETH_LINK_SPEED_20G, ROC_NIX_LINK_MODE_20000BASEMLD2_FD, CGX_MODE_20G_C2C},
	{RTE_ETH_LINK_SPEED_25G, ROC_NIX_LINK_MODE_25000BASECR_FD, CGX_MODE_25G_CR},
	{RTE_ETH_LINK_SPEED_25G, ROC_NIX_LINK_MODE_25000BASECR_FD, CGX_MODE_25GBASE_CR_C_BIT},
	{RTE_ETH_LINK_SPEED_25G, ROC_NIX_LINK_MODE_25000BASEKR_FD, CGX_MODE_25G_KR},
	{RTE_ETH_LINK_SPEED_25G, ROC_NIX_LINK_MODE_25000BASEKR_FD, CGX_MODE_25GBASE_KR_C_BIT},
	{RTE_ETH_LINK_SPEED_25G, ROC_NIX_LINK_MODE_25000BASESR_FD, CGX_MODE_25G_C2C},
	{RTE_ETH_LINK_SPEED_25G, ROC_NIX_LINK_MODE_25000BASESR_FD, CGX_MODE_25G_C2M},
	{RTE_ETH_LINK_SPEED_40G, ROC_NIX_LINK_MODE_40000BASECR4_FD, CGX_MODE_40G_CR4},
	{RTE_ETH_LINK_SPEED_40G, ROC_NIX_LINK_MODE_40000BASEKR4_FD, CGX_MODE_40G_KR4},
	{RTE_ETH_LINK_SPEED_40G, ROC_NIX_LINK_MODE_40000BASESR4_FD, CGX_MODE_40G_C2C},
	{RTE_ETH_LINK_SPEED_40G, ROC_NIX_LINK_MODE_40000BASELR4_FD, CGX_MODE_40G_C2M},
	{RTE_ETH_LINK_SPEED_50G, ROC_NIX_LINK_MODE_50000BASECR2_FD, CGX_MODE_50G_CR},
	{RTE_ETH_LINK_SPEED_50G, ROC_NIX_LINK_MODE_50000BASEKR2_FD, CGX_MODE_50G_KR},
	{RTE_ETH_LINK_SPEED_50G, ROC_NIX_LINK_MODE_50000BASESR2_FD, CGX_MODE_50G_C2C},
	{RTE_ETH_LINK_SPEED_50G, ROC_NIX_LINK_MODE_50000BASEDR_FD, CGX_MODE_50G_C2M},
	{RTE_ETH_LINK_SPEED_50G, ROC_NIX_LINK_MODE_50000BASECR_FD, CGX_MODE_50GBASE_CR2_C_BIT},
	{RTE_ETH_LINK_SPEED_50G, ROC_NIX_LINK_MODE_50000BASEKR_FD, CGX_MODE_50GBASE_KR2_C_BIT},
	{RTE_ETH_LINK_SPEED_50G, ROC_NIX_LINK_MODE_50000BASESR_FD, CGX_MODE_LAUI_2_C2C_BIT},
	{RTE_ETH_LINK_SPEED_100G, ROC_NIX_LINK_MODE_100000BASECR4_FD, CGX_MODE_100G_CR4},
	{RTE_ETH_LINK_SPEED_100G, ROC_NIX_LINK_MODE_100000BASEKR4_FD, CGX_MODE_100G_KR4},
	{RTE_ETH_LINK_SPEED_100G, ROC_NIX_LINK_MODE_100000BASESR4_FD, CGX_MODE_100G_C2C},
	{RTE_ETH_LINK_SPEED_100G, ROC_NIX_LINK_MODE_100000BASECR2_FD, CGX_MODE_100GBASE_CR2_BIT},
	{RTE_ETH_LINK_SPEED_100G, ROC_NIX_LINK_MODE_100000BASEKR2_FD, CGX_MODE_100GBASE_KR2_BIT},
	{RTE_ETH_LINK_SPEED_100G, ROC_NIX_LINK_MODE_100000BASESR2_FD, CGX_MODE_100GAUI_2_C2C_BIT},
};

static uint64_t
nix_link_advertising_get(struct cnxk_eth_dev *dev, uint32_t speed_bitmask, bool user_autoneg)
{
	struct roc_nix_mac_fwdata fwdata;
	uint64_t advertise = 0;
	uint32_t resolved = 0;
	uint32_t i;
	int rc;

	memset(&fwdata, 0, sizeof(fwdata));
	rc = roc_nix_mac_fwdata_get(&dev->nix, &fwdata);
	if (rc) {
		plt_err("Failed to get MAC firmware data");
		return 0;
	}

	/* Reject only when the user explicitly asked for autoneg. A 1G link
	 * enables autoneg internally, so it must not be blocked here.
	 */
	if (user_autoneg && !fwdata.supported_an) {
		plt_err("Autoneg is not supported");
		return 0;
	}

	for (i = 0; i < RTE_DIM(nix_link_modes); i++) {
		uint32_t speed = nix_link_modes[i].rte_speed;

		/* Skip speeds the user did not request, and speeds that are
		 * already resolved to a supported mode so the first variant wins.
		 */
		if (!(speed_bitmask & speed) || (resolved & speed))
			continue;

		/* Advertise this mode only if the firmware supports it */
		if (fwdata.supported_link_modes & BIT_ULL(nix_link_modes[i].cgx_mode)) {
			advertise |= nix_link_modes[i].adv;
			resolved |= speed;
		}
	}

	return advertise;
}

void
cnxk_nix_toggle_flag_link_cfg(struct cnxk_eth_dev *dev, bool set)
{
	if (set)
		dev->flags |= CNXK_LINK_CFG_IN_PROGRESS_F;
	else
		dev->flags &= ~CNXK_LINK_CFG_IN_PROGRESS_F;

	/* Update link info for LBK */
	if (!set &&
	    (roc_nix_is_lbk(&dev->nix) || roc_nix_is_sdp(&dev->nix) || roc_nix_is_esw(&dev->nix))) {
		struct rte_eth_link link;

		link.link_status = RTE_ETH_LINK_UP;
		link.link_speed = RTE_ETH_SPEED_NUM_100G;
		link.link_autoneg = RTE_ETH_LINK_FIXED;
		link.link_duplex = RTE_ETH_LINK_FULL_DUPLEX;
		rte_eth_linkstatus_set(dev->eth_dev, &link);
	}

	rte_wmb();
}

static inline int
nix_wait_for_link_cfg(struct cnxk_eth_dev *dev)
{
	uint16_t wait = 1000;

	do {
		rte_atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (!(dev->flags & CNXK_LINK_CFG_IN_PROGRESS_F))
			break;
		wait--;
		rte_delay_ms(1);
	} while (wait);

	return wait ? 0 : -1;
}

static void
nix_link_status_print(struct rte_eth_dev *eth_dev, struct rte_eth_link *link)
{
	if (link && link->link_status)
		plt_info("Port %d: Link Up - speed %u Mbps - %s - %s",
			 (int)(eth_dev->data->port_id),
			 (uint32_t)link->link_speed,
			 link->link_duplex == RTE_ETH_LINK_FULL_DUPLEX
				 ? "full-duplex"
				 : "half-duplex",
				 rte_eth_link_connector_to_str(link->link_connector));
	else
		plt_info("Port %d: Link Down - %s", (int)(eth_dev->data->port_id),
			 rte_eth_link_connector_to_str(link->link_connector));
}

void
cnxk_eth_dev_link_status_get_cb(struct roc_nix *nix,
				struct roc_nix_link_info *link)
{
	struct cnxk_eth_dev *dev = (struct cnxk_eth_dev *)nix;
	struct rte_eth_link eth_link;
	struct rte_eth_dev *eth_dev;

	if (!link || !nix)
		return;

	eth_dev = dev->eth_dev;
	if (!eth_dev)
		return;

	rte_eth_linkstatus_get(eth_dev, &eth_link);

	link->status = eth_link.link_status;
	link->speed = eth_link.link_speed;
	link->autoneg = eth_link.link_autoneg;
	link->full_duplex = eth_link.link_duplex;
}

void
cnxk_eth_dev_link_status_cb(struct roc_nix *nix, struct roc_nix_link_info *link)
{
	struct cnxk_eth_dev *dev = (struct cnxk_eth_dev *)nix;
	struct rte_eth_link eth_link;
	struct rte_eth_dev *eth_dev;

	if (!link || !nix)
		return;

	eth_dev = dev->eth_dev;
	if (!eth_dev || !eth_dev->data->dev_conf.intr_conf.lsc)
		return;

	if (nix_wait_for_link_cfg(dev)) {
		plt_err("Timeout waiting for link_cfg to complete");
		return;
	}

	eth_link.link_status = link->status;
	eth_link.link_speed = link->speed;
	eth_link.link_autoneg = link->autoneg ? RTE_ETH_LINK_AUTONEG : RTE_ETH_LINK_FIXED;
	eth_link.link_duplex = link->full_duplex;
	eth_link.link_connector = dev->link_type;

	/* Print link info */
	nix_link_status_print(eth_dev, &eth_link);

	/* Update link info */
	rte_eth_linkstatus_set(eth_dev, &eth_link);

	/* Set the flag and execute application callbacks */
	rte_eth_dev_callback_process(eth_dev, RTE_ETH_EVENT_INTR_LSC, NULL);
}

int
cnxk_nix_link_update(struct rte_eth_dev *eth_dev, int wait_to_complete)
{
	struct cnxk_eth_dev *dev = cnxk_eth_pmd_priv(eth_dev);
	struct roc_nix_link_info info;
	struct rte_eth_link link;
	int rc;

	RTE_SET_USED(wait_to_complete);
	memset(&link, 0, sizeof(struct rte_eth_link));

	if (!eth_dev->data->dev_started)
		return 0;

	if (roc_nix_is_lbk(&dev->nix) || roc_nix_is_sdp(&dev->nix)) {
		link.link_status = RTE_ETH_LINK_UP;
		link.link_speed = RTE_ETH_SPEED_NUM_100G;
		link.link_autoneg = RTE_ETH_LINK_FIXED;
		link.link_duplex = RTE_ETH_LINK_FULL_DUPLEX;
	} else {
		rc = roc_nix_mac_link_info_get(&dev->nix, &info);
		if (rc)
			return rc;
		link.link_status = info.status;
		link.link_speed = info.speed;
		link.link_autoneg = info.autoneg ? RTE_ETH_LINK_AUTONEG : RTE_ETH_LINK_FIXED;
		if (info.full_duplex)
			link.link_duplex = info.full_duplex;
		link.link_connector = dev->link_type;
	}

	return rte_eth_linkstatus_set(eth_dev, &link);
}

int
cnxk_nix_link_info_configure(struct rte_eth_dev *eth_dev)
{
	uint32_t speed_map[] = {
		RTE_ETH_SPEED_NUM_NONE, RTE_ETH_SPEED_NUM_10M,  RTE_ETH_SPEED_NUM_10M,
		RTE_ETH_SPEED_NUM_100M, RTE_ETH_SPEED_NUM_100M, RTE_ETH_SPEED_NUM_1G,
		RTE_ETH_SPEED_NUM_2_5G, RTE_ETH_SPEED_NUM_5G,   RTE_ETH_SPEED_NUM_10G,
		RTE_ETH_SPEED_NUM_20G,  RTE_ETH_SPEED_NUM_25G,  RTE_ETH_SPEED_NUM_40G,
		RTE_ETH_SPEED_NUM_50G,  RTE_ETH_SPEED_NUM_56G,  RTE_ETH_SPEED_NUM_100G,
		RTE_ETH_SPEED_NUM_200G, RTE_ETH_SPEED_NUM_400G
	};
	struct cnxk_eth_dev *dev = cnxk_eth_pmd_priv(eth_dev);
	struct rte_eth_dev_data *data = eth_dev->data;
	struct rte_eth_conf *conf = &data->dev_conf;
	uint32_t link_speeds = conf->link_speeds;
	struct roc_nix_link_info link_info = {0};
	struct roc_nix *nix = &dev->nix;
	uint32_t speed = link_speeds;
	bool user_autoneg;
	bool fixed;
	int rc;

	plt_info("User passed link configuration: %x", link_speeds);

	if (!roc_nix_is_pf(nix) || link_speeds == RTE_ETH_LINK_SPEED_AUTONEG)
		return 0;

	fixed = link_speeds & RTE_ETH_LINK_SPEED_FIXED ? true : false;
	if (fixed) {
		if (rte_popcount32(link_speeds) == 1) {
			plt_err("Desired speed is not specified in FIXED mode");
			return -EINVAL;
		}

		if (rte_popcount32(link_speeds) > 2) {
			plt_err("Multiple speeds can't be configured in FIXED mode");
			return -EINVAL;
		}

		link_info.autoneg = 0;
	} else {
		link_info.autoneg = 1;
	}
	user_autoneg = !fixed;

	speed >>= 1;
	link_info.speed = speed_map[rte_bsf32(speed) + 1];
	link_info.speed_bitmask = link_speeds & ~RTE_ETH_LINK_SPEED_FIXED;
	link_info.full_duplex = ((link_speeds & RTE_ETH_LINK_SPEED_10M_HD) ||
				 (link_speeds & RTE_ETH_LINK_SPEED_100M_HD)) ?
					ROC_NIX_LINK_DUPLEX_HALF :
					ROC_NIX_LINK_DUPLEX_FULL;

	/* A 1G link requires in-band negotiation with the peer during link bring up,
	 * so autoneg must be enabled even when the user requested a FIXED speed.
	 */
	if ((link_info.speed_bitmask & RTE_ETH_LINK_SPEED_1G) && !link_info.autoneg) {
		plt_info("Forcing autoneg for 1G");
		link_info.autoneg = 1;
	}

	link_info.advertising =
		nix_link_advertising_get(dev, link_info.speed_bitmask, user_autoneg);
	if (link_info.advertising == 0) {
		plt_err("advertising bitmap is not set");
		return -EINVAL;
	}

	plt_info("Following link settings are sent to firmware:");
	plt_info("Advertised modes: %" PRIX64, link_info.advertising);
	plt_info("speed: %u", link_info.speed);
	plt_info("duplex: %s",
		 link_info.full_duplex == ROC_NIX_LINK_DUPLEX_HALF ? "half-duplex" : "full-duplex");
	plt_info("autoneg: %s", link_info.autoneg ? "enabled" : "disabled");

	/* Bring the link down and up around the mode change so that the SerDes
	 * retrains at the newly requested rate. These toggles are not fatal.
	 */
	rc = roc_nix_mac_link_state_set(nix, false);
	if (rc)
		plt_warn("Failed to bring link down before reconfigure, rc=%d", rc);

	rc = roc_nix_mac_link_info_set(nix, &link_info);
	if (rc) {
		plt_err("Failed to set link mode, rc=%d", rc);
		/* Try to restore the link after a failed mode change */
		roc_nix_mac_link_state_set(nix, true);
		return rc;
	}

	rc = roc_nix_mac_link_state_set(nix, true);
	if (rc)
		plt_warn("Failed to bring link up after reconfigure, rc=%d", rc);

	return 0;
}
