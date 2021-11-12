/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#include "cn9k_ethdev.h"
#include "cn9k_rx.h"

#define R(name, flags) NIX_RX_RECV_MSEG(cn9k_nix_recv_pkts_mseg_##name, flags)

NIX_RX_FASTPATH_MODES_64_79
#undef R