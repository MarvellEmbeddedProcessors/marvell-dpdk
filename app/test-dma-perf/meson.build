# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2019-2023 Intel Corporation

# meson file, for building this app as part of a main DPDK build.

if is_windows
    build = false
    reason = 'not supported on Windows'
    subdir_done()
endif

deps += ['dmadev', 'mbuf', 'cfgfile']

sources = files(
        'main.c',
        'benchmark.c',
)
