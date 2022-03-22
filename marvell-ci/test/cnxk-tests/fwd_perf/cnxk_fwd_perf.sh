#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(C) 2021 Marvell.

set -e

CNXKTESTPATH="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"
VFIO_DEVBIND="$1/marvell-ci/test/board/oxk-devbind-basic.sh"

source $CNXKTESTPATH/../common/testpmd/common.env

# Find the dpdk-ipsec-secgw application
if [[ -f $CNXKTESTPATH/../../../../app/dpdk-testpmd ]]; then
	# This is running from build directory
	PATH="$CNXKTESTPATH/../../../../app:$PATH"
	PATH="$CNXKTESTPATH/../../../../examples:$PATH"
elif [[ -f $CNXKTESTPATH/../../dpdk-testpmd ]]; then
	# This is running from install directory
	PATH="$CNXKTESTPATH/../..:$PATH"
else
	TESTPMD_BIN=$(which dpdk-testpmd)
	L3FWD_BIN=$(which dpdk-l3fwd)
	L2FWD_BIN=$(which dpdk-l2fwd)
	if [[ -z $TESTPMD_BIN ]] || \
		[[ -z $L3FWD_BIN ]] || [[ -z $L2FWD_BIN ]]; then
		echo "dpdk-testpmd|dpdk-l2fwd|dpdk-l3fwd not found !!"
		exit 1
	fi
fi

declare -i num_tests
declare -a test_name
declare -a test_cmd
declare -a test_args
declare -a test_eal_args

SUDO="sudo"
PRFX="fwd_perf"
remote_ssh="${TARGET_SSH_CMD:-"ssh -o LogLevel=ERROR -o ServerAliveInterval=30 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"} $GENERATOR_BOARD"
gen=$(realpath ./cnxk_fwd_gen.sh)
MAX_RETRY=${MAX_RETRY:-5}
WITH_GEN_BOARD=0
GEN_ARG=$1
G_ENV=
TOLERANCE=${TOLERANCE:-5}

FWD_PERF_IN=fwd_perf.in
FWD_PERF_OUT=fwd_perf.out
FWD_PERF_OUT_FULL=fwd_perf.out.full
GEN_LOG_FULL=gen.out.full

START_STR=">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
END_STR="<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<"


if [[ -z "$GENERATOR_BOARD" ]]; then
	echo "Generator board details missing!!"
	WITH_GEN_BOARD=0
fi

if [[ $WITH_GEN_BOARD -eq 0 ]]
then
	IF0=0002:01:00.1
	IF1=0002:01:00.2
	remote_ssh="sh -c "
	GEN_PORT=$IF1
	G_ENV="GEN_CORES=4"
	SUDO=""
	echo "Running locally without generator board"
else
	IF0=0002:02:00.0
	GEN_PORT=$IF0
	$VFIO_DEVBIND -b vfio-pci $IF0
	# Dummy whitelist device
	IF1=0008:08:08.0
	echo "Running with generator board"
fi

rm -rf $FWD_PERF_IN $FWD_PERF_OUT $FWD_PERF_OUT_FULL $GEN_LOG_FULL

function sig_handler()
{
	local status=$?
	set +e
	trap - ERR
	trap - INT
	trap - QUIT
	trap - EXIT
	if [[ $status -ne 0 ]]; then
		echo "$1 Handler"
	fi

	awk ' { print FILENAME": " $0 } ' $FWD_PERF_OUT_FULL
	awk ' { print FILENAME": " $0 } ' $FWD_PERF_OUT
	awk ' { print FILENAME": " $0 } ' $GEN_LOG_FULL

	killall -9 dpdk-l3fwd dpdk-l2fwd dpdk-testpmd
	$remote_ssh "sudo killall -9 dpdk-testpmd"
	exit $status
}

trap "sig_handler ERR" ERR
trap "sig_handler INT" INT
trap "sig_handler QUIT" QUIT
trap "sig_handler EXIT" EXIT

# Get CPU PART NUMBER
PARTNUM=$(grep -m 1 'CPU part' /proc/cpuinfo | grep -o '0x0[a-b][0-3]$')
if [[ $PARTNUM == $PARTNUM_98XX ]]; then
	HW="cn98"
else
	HW="cn96"
fi

# get chip number and RCLK
function get_system_info()
{
	local sysclk_dir
	local fp_rclk
	local fp_sclk
	local div=1000000

	sysclk_dir="/sys/kernel/debug/clk"
	fp_rclk="$sysclk_dir/rclk/clk_rate"
	fp_sclk="$sysclk_dir/sclk/clk_rate"

	if $SUDO test -f "$fp_rclk"; then
		RCLK=$(echo "`$SUDO cat $fp_rclk` / $div" | bc)
	else
		echo "$fp_rclk not available"
		exit 1
	fi

	if $SUDO test -f "$fp_sclk"; then
		SCLK=$(echo "`$SUDO cat $fp_sclk` / $div" | bc)
	else
		echo "$fp_sclk not available"
		exit 1
	fi

	echo "RCLK:   $RCLK Mhz"
	echo "SCLK:   $SCLK Mhz"
}

register_fwd_test() {
        test_name[$num_tests]=$1
	test_cmd[$num_tests]=$2
	test_eal_args[$num_tests]=$3
        test_args[$num_tests]=$4
        ((num_tests+=1))
}

expected_pps() {
	FNAME="rclk"${RCLK}"_sclk"${SCLK}"."${HW}"."${test_cmd[$1]}
	FPATH="$CNXKTESTPATH/ref_numbers/$FNAME"
	if [[ ! -f $FPATH ]]; then echo 'Err: ref file missing !!'; exit 1; fi

	pps_gold=$(grep "${test_name[$1]}" $FPATH \
			| tr -s ' ' | cut -d " " -f 2)
	echo "($pps_gold * (100 - $TOLERANCE)) / 100" | bc
}

ref_pps() {
	FNAME="rclk"${RCLK}"_sclk"${SCLK}"."${HW}"."${test_cmd[$1]}
	FPATH="$CNXKTESTPATH/ref_numbers/$FNAME"
	if [[ ! -f $FPATH ]]; then echo 'Err: ref file missing !!'; exit 1; fi

	pps_gold=$(grep "${test_name[$1]}" $FPATH \
			| tr -s ' ' | cut -d " " -f 2)
	echo $pps_gold
}

launch_gen() {
	echo $START_STR ${test_name[$1]} >>$GEN_LOG_FULL
	$remote_ssh "$SUDO PORT0=$GEN_PORT TEST_OP=launch $G_ENV $gen $GEN_ARG"
}

start_gen() {
	$remote_ssh "$SUDO PORT0=$GEN_PORT TEST_OP=start $gen"
}

stop_gen() {
	$remote_ssh "$SUDO PORT0=$GEN_PORT TEST_OP=stop $gen"
}

cleanup_gen() {
	$remote_ssh "$SUDO PORT0=$GEN_PORT TEST_OP=log $gen" >>$GEN_LOG_FULL
	echo $END_STR ${test_name[$idx]} >>$GEN_LOG_FULL

	$remote_ssh "$SUDO PORT0=$GEN_PORT TEST_OP=cleanup $gen"
}

testpmd_pps_local() {
	local rx_pps=0

	echo "show port stats all" >>$FWD_PERF_IN
	sleep 1
	echo "show port stats all" >>$FWD_PERF_IN
	while ! (tail -n1 $FWD_PERF_OUT | grep -q "testpmd> $")
	do
		sleep 0.1
		continue;
	done

	pps=`cat $FWD_PERF_OUT | tail -16 | \
		grep "Rx-pps:" | awk -e '{ print $2 }'`
	for i in $pps
	do
		rx_pps=$((rx_pps + i))
	done
	echo $rx_pps
}

check_pps() {
	idx=$1
	pass_pps=$(expected_pps $idx)
	ref_pps=$(ref_pps $idx)
	local retry=3

	while [[ retry -ne 0 ]]
	do

		if [[ $WITH_GEN_BOARD -eq 0 ]] && [[ "${test_cmd[$idx]}" == "testpmd" ]]
		then
			rx_pps=$(testpmd_pps_local)
		else
			rx_pps=$($remote_ssh "$SUDO TEST_OP=rx_pps $gen")
		fi

		if [[ rx_pps -lt pass_pps ]]; then
			echo -n "Low PPS for ${test_name[$idx]} ($rx_pps < $pass_pps)"
			echo " (Ref $ref_pps, tolerance $TOLERANCE%)"
		else
			echo -n "Rx PPS $rx_pps as expected $pass_pps"
			echo " (Ref $ref_pps, tolerance $TOLERANCE%)"
			return 0
		fi

		sleep 1
		((retry-=1))
	done

	return 1
}

cleanup_one() {
	local idx=$1
	local cmd=${test_cmd[$idx]}

	# Save pid
	PID=`ps -eo "pid,args" | grep $cmd | grep $PRFX | awk '{print $1}'`

        # Issue kill
        ps -eo "pid,args" | grep $cmd | grep $PRFX | \
                awk '{print $1}' | xargs -I[] -n1 kill -2 [] 2>/dev/null || true

        # Wait until the process is killed
        while (ps -ef | grep $cmd | grep -q $PRFX) || (kill -0 $PID)
	do
		echo $?
		sleep 0.1
                continue
        done

	if [[ $WITH_GEN_BOARD -eq 1 ]] || [[ "${test_cmd[$idx]}" != "testpmd" ]]
	then
		stop_gen
		cleanup_gen $idx
	fi

	cat $FWD_PERF_OUT >> $FWD_PERF_OUT_FULL
	echo $END_STR ${test_name[$idx]} >>$FWD_PERF_OUT_FULL
}

run_one() {
	unbuffer="$(command -v stdbuf) -o 0" || unbuffer=
	local in=$FWD_PERF_IN
	local out=$FWD_PERF_OUT
	idx=$1

	echo $START_STR ${test_name[$idx]} >>$FWD_PERF_OUT_FULL

	rm -rf $in $out
	touch $in $out

	cmd=${test_cmd[$idx]}

	case $cmd in
	testpmd)
		if ! command -v dpdk-testpmd; then
			echo "dpdk-testpmd not found"
			exit 1
		fi

		echo -n "Starting testpmd with '-c 0xff0000 -a $IF0 -a $IF1 "
		echo " ${test_eal_args[$idx]} -- ${test_args[$idx]} -i'"

		tail -f $in | \
			$unbuffer dpdk-testpmd -c 0xff0000 \
			--file-prefix $PRFX -a $IF0 -a $IF1 \
			${test_eal_args[$idx]} -- \
			${test_args[$idx]} -i &>$out 2>&1 &
		# Wait for testpmd to be up
		itr=0
		while ! (tail -n1 $out | grep -q "testpmd> $")
		do
			sleep 0.1
			((itr+=1))
			if [[ itr -eq 1000 ]]
			then
				echo "Timeout waiting for testpmd";
				exit 1;
			fi
			continue;
		done
		# Disable flow control
		echo "set flow_ctrl rx off 0" >>$FWD_PERF_IN
		echo "set flow_ctrl tx off 0" >>$FWD_PERF_IN
		echo "set flow_ctrl rx off 1" >>$FWD_PERF_IN
		echo "set flow_ctrl tx off 1" >>$FWD_PERF_IN

		echo "start tx_first 256" >>$FWD_PERF_IN

		if [[ $WITH_GEN_BOARD -eq 1 ]]
		then launch_gen $idx; start_gen; fi
		;;
	l3fwd)
		if ! command -v dpdk-l3fwd; then
			echo "dpdk-l3fwd not found"
			exit 1
		fi

		echo -n "Starting l3fwd with '-c 0xff0000 -a $IF0 "
		echo " ${test_eal_args[$idx]} -- ${test_args[$idx]}'"

		tail -f $in | \
			$unbuffer dpdk-l3fwd --file-prefix $PRFX \
			-c 0xff0000 -a $IF0 ${test_eal_args[$idx]} -- \
			${test_args[$idx]} &>$out 2>&1 &
		# Wait for l3fwd to be up
		itr=0
		while ! (tail -n20 $out | grep -q "L3FWD: entering main loop")
		do
			sleep 0.1
			((itr+=1))
			if [[ itr -eq 10000 ]]
			then
				echo "Timeout waiting for l3fwd";
				exit 1;
			fi
			continue;
		done
		launch_gen $idx
		start_gen
		;;
	l2fwd)
		if ! command -v dpdk-l2fwd; then
			echo "dpdk-l2fwd not found"
			exit 1
		fi

		echo -n "Starting l2fwd with '-c 0xff0000 -a $IF0 "
		echo " ${test_eal_args[$idx]} -- ${test_args[$idx]} -i'"

		tail -f $in | \
			$unbuffer dpdk-l2fwd --file-prefix $PRFX \
			-c 0xff0000 -a $IF0 ${test_eal_args[$idx]} -- \
			${test_args[$idx]} &>$out 2>&1 &
		# Wait for l2fwd to be up
		itr=0
		while ! (tail -n20 $out | grep -q "L2FWD: entering main loop")
		do
			sleep 0.1
			continue;
			((itr+=1))
			if [[ itr -eq 1000 ]]
			then
				echo "Timeout waiting for l2fwd";
				exit 1;
			fi
		done
		launch_gen $idx
		start_gen
		;;
	*)
		echo "Unknown test command $cmd"
		exit 1
		;;
	esac
}

run_fwd_tests() {

	get_system_info

	idx=0
	ret=0
	local retry_count=$MAX_RETRY
	while [[ idx -lt num_tests ]]; do
		run_one $idx

		sleep 3

		set +e
		check_pps $idx
		local k=$?
		set -e

		if [[ k -eq 0 ]]; then
			cleanup_one $idx

			((idx+=1))
			retry_count=$MAX_RETRY
			continue
		fi
		((retry_count-=1)) || true

		if [[ retry_count -eq 0 ]]; then
			echo "FAIL: ${test_name[$idx]}"
			cleanup_one $idx

			((ret+=1))
			((idx+=1))
			retry_count=$MAX_RETRY
		else
			echo "Re-run ${test_name[$idx]} $retry_count"
			cleanup_one $idx
		fi
	done

	exit $ret
}

num_tests=0

# Register fwd performance tests.
# Format:		<test name>		<cmd>          <args>
register_fwd_test "TESTPMD_NO_OFFLOAD" "testpmd" "" \
			"--no-flush-rx --nb-cores=1"

register_fwd_test "L3FWD_1C"          "l3fwd"   "" \
			  "-p 0x1 --config (0,0,23) -P"

register_fwd_test "L2FWD_1C"          "l2fwd"   "" \
			  "-p 0x1 -T 0"

run_fwd_tests

cleanup_gen