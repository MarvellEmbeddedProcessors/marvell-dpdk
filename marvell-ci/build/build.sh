#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2021 Marvell.
#
# Script will build DPDK in <build-root>/build and install in <build-root>/prefix
#

set -euo pipefail
set -x

function help() {
	set +x
	echo "Build DPDK Library"
	echo ""
	echo "Usage:"
	echo "$SCRIPT_NAME [ARGUMENTS]..."
	echo ""
	echo "Mandatory Arguments"
	echo "==================="
	echo "--build-root | -r            : Build root directory"
	echo "--build-env | -b             : Build Environment"
	echo ""
	echo "Optional Arguments"
	echo "==================="
	echo "--extra-meson-args | -m      : Additional arguments to meson"
	echo "--jobs | -j                  : Number of parallel jobs [Default: 4]"
	echo "--project-root | -p          : DPDK Project root [Default: PWD]"
	echo "--no-clean | -n              : Dont clean build directories before building"
	echo "--verbose | -v               : Enable verbose logs"
	echo "--help | -h                  : Print this help and exit"
	set -x
}

SCRIPT_NAME="$(basename "$0")"
if ! OPTS=$(getopt \
	-o "r:b:m:j:p:hvn" \
	-l "build-root:,build-env:,extra-meson-args:,jobs:,project-root:,help,verbose,no-clean" \
	-n "$SCRIPT_NAME" \
	-- "$@"); then
	help
	exit 1
fi

BUILD_ROOT=
BUILD_ENV=
MAKE_J=4
EXTRA_ARGS=
PROJECT_ROOT="$PWD"
VERBOSE=
DONT_CLEAN=

eval set -- "$OPTS"
unset OPTS
while [[ $# -gt 1 ]]; do
	case $1 in
		-r|--build-root) shift; BUILD_ROOT=$1;;
		-b|--build-env) shift; BUILD_ENV=$(realpath $1);;
		-m|--extra-meson-args) shift; EXTRA_ARGS="$1";;
		-j|--jobs) shift; MAKE_J=$1;;
		-p|--project-root) shift; PROJECT_ROOT=$1;;
		-n|--no-clean) DONT_CLEAN=1;;
		-v|--verbose) VERBOSE='-v';;
		-h|--help) help; exit 0;;
		*) help; exit 1;;
	esac
	shift
done


if [[ -z $BUILD_ROOT || -z $BUILD_ENV ]]; then
	echo "Build root directory and build env should be passed !!"
	help
	exit 1
fi

PROJECT_ROOT=$(realpath $PROJECT_ROOT)
mkdir -p $BUILD_ROOT
BUILD_ROOT=$(realpath $BUILD_ROOT)
BUILD_DIR=$BUILD_ROOT/build
PREFIX_DIR=$BUILD_ROOT/prefix

source $BUILD_ENV

if [[ -z $DONT_CLEAN ]]; then
	rm -rf $BUILD_DIR
	rm -rf $PREFIX_DIR
fi

cd $PROJECT_ROOT

# Do any pre-build stuff
${BUILD_SETUP_CMD:-}

meson $BUILD_DIR \
	--prefix $PREFIX_DIR \
	$EXTRA_ARGS \
	-Dc_args="${CFLAGS:-}" \
	-Dc_link_args="${LDFLAGS:-}"

ninja -C $BUILD_DIR -j $MAKE_J $VERBOSE
ninja -C $BUILD_DIR -j $MAKE_J $VERBOSE install

