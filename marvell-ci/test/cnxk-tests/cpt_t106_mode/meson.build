# SPDX-License-Identifier: BSD-3-Clause
# Copyright(C) 2023 Marvell.

# Test script
test_script = 'cpt_t106_mode_test.sh'

# Test name
test_name = 'cpt_t106_mode'

# Test arguments
test_args = ''

# Test directory name relative to build directory.
test_dir = meson.current_build_dir()

# Copy the required scripts to build directory.
run_command(copy_data, test_script)

# Add the meson event test
test(
    test_name,
    cnxk_test_script_wrapper,
    env : ['DPDK_TEST=' + test_name, 'TEST_DIR=' + test_dir],
    args : [test_script, test_args],
    is_parallel : false,
    suite : 'cnxk-tests')

# Install the sample script
install_data(
    'cpt_t106_mode_test.sh',
    install_mode : 'rwxr-xr-x',
    install_dir : 'bin/cnxk/cpt_t106_mode')
