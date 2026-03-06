# SPDX-License-Identifier: BSD-3-Clause

if is_windows
    build = false
    reason = 'not supported on Windows'
    subdir_done()
endif

sources = files('xgmac_ethdev.c')
deps += ['bus_platform']

require_iova_in_mbuf = false
