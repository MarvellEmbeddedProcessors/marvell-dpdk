#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2021 Marvell.
set -eou pipefail
CNXKTESTPATH="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )/.."
source $CNXKTESTPATH/common/testpmd/common.env
IF0=${IF0:-0002:02:00.0}
PRFX="event_perf"

trap "cleanup $?" EXIT

cleanup()
{
	if [[ $1 -ne 0 ]]; then
		testpmd_cleanup $PRFX
	fi

	exit $1
}

launch_testpmd()
{
	local fwd_cores=$(($(grep -c ^processor /proc/cpuinfo) - 1))
	testpmd_launch $PRFX \
		"-l 0-$fwd_cores -a $IF0" \
		"--nb-cores=$fwd_cores --rxq=$fwd_cores --txq=$fwd_cores --forward-mode=flowgen" \
		</dev/null 2>/dev/null &
	testpmd_cmd $PRFX "set flow_ctrl rx off 0"
	testpmd_cmd $PRFX "set flow_ctrl tx off 0"

}

case $TESTPMD_OP in
	launch)
		launch_testpmd
		;;
	start)
		testpmd_cmd $PRFX "start tx_first 64"
		testpmd_cmd $PRFX "show port stats all"
		;;
	stop)
		testpmd_cmd $PRFX "show port stats all"
		testpmd_cmd $PRFX "stop";;
	rx_pps)
		testpmd_cmd $PRFX "show port stats all"
		val=`testpmd_log $PRFX | tail -4 | grep -ao 'Rx-pps: .*' \
			| awk -e '{print $2}'`
		echo $val
		;;
	cleanup)
		testpmd_cleanup $PRFX;;
esac