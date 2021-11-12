/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#include "cn9k_worker.h"

#define T(name, sz, flags)                                                     \
	SSO_DUAL_TX(cn9k_sso_hws_dual_tx_adptr_enq_##name, sz, flags)

NIX_TX_FASTPATH_MODES_48_63
#undef T