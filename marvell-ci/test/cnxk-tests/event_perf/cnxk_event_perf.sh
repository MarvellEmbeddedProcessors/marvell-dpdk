#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2021 Marvell.
set -euo pipefail

GENERATOR_BOARD=${GENERATOR_BOARD:?}
REMOTE_DIR=${REMOTE_DIR:-$(pwd | cut -d/ -f 1-3)}
CORES=(1 8 16)
IF0=${IF0:-0002:02:00.0}
IF1=${IF1:-0002:01:00.1}
IF2=${IF2:-0002:01:00.2}
EVENT_DEV=${EVENT_DEV:-0002:0e:00.0}
CRYPTO_DEV=${CRYPTO_DEV:-0002:10:00.1}
TOLERANCE=${TOLERANCE:-5}
MAX_RETRY_COUNT=${MAX_RETRY_COUNT:-5}
GENERATOR_SCRIPT=${GENERATOR_SCRIPT:-cnxk_event_perf_gen.sh}
REF_FILE=${REF_FILE:-ref_numbers/cn96xx_rclk2200_sclk1100.csv}
TARGET_SSH_CMD=${TARGET_SSH_CMD:-"ssh -o LogLevel=ERROR -o ServerAliveInterval=30 \
	-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"}
TARGET_SSH_CMD="$TARGET_SSH_CMD -n"
RES_SEPERATOR="------------------------------------------------------------------------"

trap 'cleanup $?' EXIT

cleanup()
{
	exec_testpmd_cmd cleanup
	exit $1
}

get_test_args()
{
	local test_name=$1
	local num_cores=$2
	local sched_mode=$3

	case $test_name in
		L2FWD_EVENT)
			echo "-l 0-$num_cores -n 4 -a $IF0 -a $EVENT_DEV -- -p 1" \
				"--mode=eventdev --eventq-sched=${sched_mode:0:1}"
			;;
		L3FWD_EVENT)
			echo "-l 0-$((num_cores - 1)) -n 4 -a $IF0 -a $EVENT_DEV -- -p 1" \
				"--mode=eventdev --eventq-sched=$sched_mode -P"
			;;
		PERF_ATQ)
			echo "-l 0-$num_cores -n 4 -a $IF0 -a $EVENT_DEV --" \
				"--prod_type_ethdev --nb_pkts=0 --test=perf_atq" \
				"--stlist=${sched_mode:0:1} --wlcores=1-$num_cores"
			;;
		PIPELINE_ATQ_TX_FIRST)
			echo "-l 0-$num_cores -n 4 -a $IF1 -a $IF2 -a $EVENT_DEV -- " \
				"--prod_type_ethdev --nb_pkts=0 --verbose 2" \
				"--test=pipeline_atq --stlist=${sched_mode:0:1}"\
				"--wlcores=1-$num_cores --tx_first 256"
			;;
		CRYPTO_ADAPTER_FWD)
			local req_cores=$((num_cores * 2))
			local max_cores=$(($(grep -c ^processor /proc/cpuinfo) - 1))
			((req_cores = req_cores>max_cores?max_cores:req_cores))
			echo "-l 0-$req_cores -n 4 -a $CRYPTO_DEV -a $EVENT_DEV --" \
				"--prod_type_cryptodev --crypto_adptr_mode 1 --nb_flows=100" \
				"--nb_pkts=0 --test=perf_atq --stlist=${sched_mode:0:1}" \
				"--plcores=1-$num_cores --wlcores=$((num_cores + 1))-$req_cores"
			;;
	esac
}

find_exec()
{
	local dut=$1
	local test_name=$2

	$TARGET_SSH_CMD $dut find $REMOTE_DIR -type f -executable -iname $test_name
}

exec_testpmd_cmd()
{
	$TARGET_SSH_CMD $GENERATOR_BOARD "cd $REMOTE_DIR;" \
		"sudo TESTPMD_OP=$1 $(find_exec $GENERATOR_BOARD $GENERATOR_SCRIPT)"
}

launch_testpmd()
{
	$TARGET_SSH_CMD $GENERATOR_BOARD "cd $REMOTE_DIR;" \
		"sudo TESTPMD_OP=launch $(find_exec $GENERATOR_BOARD $GENERATOR_SCRIPT)"
}

gen_needed()
{
	local test_name=$1

	case $test_name in
		CRYPTO_ADAPTER_FWD)
			return 1
			;;
		*)
			return 0
			;;
	esac
}

record_pps()
{
	local test_bin=$1
	local test_log=$2
	local out_file=$3

	case $test_bin in
		dpdk-l2fwd-event | dpdk-l3fwd)
			printf "$(exec_testpmd_cmd rx_pps)\n" >> $out_file
			;;
		dpdk-test-eventdev)
			pps=$(sed -n "s/.\+ mpps avg \([0-9\.]\+\) mpps/\1/p" $test_log \
				| sed "s/[^[:print:]].*//")
			printf "$pps\n" >> $out_file
			;;
	esac
}

kill_test()
{
	local test_bin=$1

	while pgrep -f $test_bin > /dev/null; do
		disown $(pgrep -f $test_bin) &>/dev/null
		sudo killall -q -9 $test_bin
		timeout --foreground -k 30 -s 3 wait $(pgrep -f $test_bin) &>/dev/null
	done
	sleep 5
}

measure_test_perf()
{
	local test_name=$1
	local test_bin=$2
	local pattern="$3"
	local sched_mode=$4
	local out_file=$5
	local test_log=$(mktemp)
	local unbuffer="stdbuf -o0"
	local test_path=$(find $REMOTE_DIR -type f -executable -iname $test_bin)

	printf "cores, RESULT\n" >> $out_file
	launch_testpmd

	for num_cores in "${CORES[@]}"; do
		printf "$num_cores, " >> $out_file
		test_args=$(get_test_args $test_name $num_cores $sched_mode)
		sudo $unbuffer $test_path $test_args &> $test_log &
		while ! (tail -n1 $test_log | grep -q "$pattern"); do
			sleep 1
		done

		if gen_needed $test_name; then
			exec_testpmd_cmd "start"
		fi

		sleep 15 # run test for 15 sec
		record_pps $test_bin $test_log $out_file

		if gen_needed $test_name; then
			exec_testpmd_cmd "stop"
		fi

		kill_test $test_bin
	done

	exec_testpmd_cmd cleanup
	rm $test_log
}

print_comparison_sheet()
{
	local ref_file=$1
	local out_file=$2
	local tmp_file=$(mktemp)

	sed "s/RESULT/ref num, new num/" $ref_file > $tmp_file

	sed -e 's/[0-9]\+, \([0-9\., ]\+\)/, \1/' -e 's/^[^,].*//' $out_file \
		| paste -d '' $tmp_file -

	rm $tmp_file
}

check_regression()
{
	local ref_file=$1
	local cur_file=$2
	local ref_num=$(mktemp)
	local cur_num=$(mktemp)
	local ret=0

	sed -n 's/.*, \([0-9\.]\+\)$/\1/p' $ref_file > $ref_num
	sed -n 's/.*, \([0-9\.]\+\)$/\1/p' $cur_file > $cur_num

	while IFS=$'\t' read -r rn cn; do
		if (( $(echo "($rn * (100 - $TOLERANCE) / 100) > $cn" | bc -l) )); then
			ret=1
			break
		fi
	done < <(paste $ref_num $cur_num)

	if [[ ret -eq 0 ]]; then
		printf "No regression found\n\n"
	else
		printf "Found regression for previous run, ($cn < $rn)\n\n"
	fi

	rm $ref_num $cur_num
	return $ret
}

get_sched_modes()
{
	test_name=$1

	case $test_name in
		CRYPTO_ADAPTER_FWD)
			echo "parallel"
			;;
		*)
			echo "parallel atomic ordered"
			;;
	esac
}

run_event_perf_test()
{
	local test_name=$1
	local test_bin=$2
	local test_pattern="$3"
	local ref_file=$(mktemp)
	local out_file=$(mktemp)
	local ret

	for sched_mode in $(get_sched_modes $test_name); do
		printf "$test_name ($sched_mode queue) output in packets per second\n" >> $out_file
		measure_test_perf $test_name $test_bin "$test_pattern" $sched_mode $out_file
	done
	printf "\n$RES_SEPERATOR\n" >> $out_file

	awk "/$test_name/ {p=1}; p; /$RES_SEPERATOR/ {p=0}" $REF_FILE >> $ref_file
	print_comparison_sheet $ref_file $out_file
	check_regression $ref_file $out_file
	ret=$?

	rm $ref_file $out_file
	return $ret
}

run_event_perf_regression()
{
	# Test info in <test_name>\t<test_bin>\t<test_pattern> format
	local test_info="
	L2FWD_EVENT		dpdk-l2fwd-event	=*
	L3FWD_EVENT		dpdk-l3fwd		L3FWD: entering lpm_event_loop_single on lcore [0-9]*
	PERF_ATQ		dpdk-test-eventdev	0.000 mpps avg 0.000 mpps
	PIPELINE_ATQ_TX_FIRST	dpdk-test-eventdev	[0-9]*\.[0-9]* mpps avg [0-9]*\.[0-9]* mpps
	CRYPTO_ADAPTER_FWD	dpdk-test-eventdev	[0-9]*\.[0-9]* mpps avg [0-9]*\.[0-9]* mpps"

	while IFS= read -r test; do
		local retry_count=$MAX_RETRY_COUNT

		if [[ -z $test ]]; then
			continue
		fi

		IFS=$'\t' read -r -a info <<< "$test"
		printf "Checking ${info[0]} numbers\n"
		while ! run_event_perf_test ${info[0]} ${info[1]} "${info[2]}"; do
			((retry_count--))
			if [[ retry_count -gt 0 ]]; then
				printf "Retry checking ${info[0]} numbers\n"
			else
				printf "${info[0]} regression check failed\n"
				exit 1
			fi
		done
	done <<< "$test_info"
}

run_event_perf_regression