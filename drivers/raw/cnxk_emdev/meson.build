# SPDX-License-Identifier: BSD-3-Clause
# Copyright(C) 2025 Marvell.
#

if not dpdk_conf.get('RTE_ARCH_64')
    build = false
    reason = 'only supported on 64-bit'
    subdir_done()
endif

sources = files('cnxk_emdev.c')

deps += ['rawdev', 'mbuf', 'mempool', 'pci', 'bus_pci']
deps += ['common_cnxk', 'mempool_cnxk']
headers = files('rte_pmd_cnxk_emdev.h')

require_iova_in_mbuf = false
