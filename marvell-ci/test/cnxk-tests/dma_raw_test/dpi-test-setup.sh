#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 Marvell.

# Bind Marvell DPI device(s) + NPA PF to vfio-pci and allocate hugepages.
#
# CN10K (177d:a080/a081): kernel DPI PF + SRIOV VFs; bind VFs and NPA PF only.
# CN20K (177d:a0e8/a0e9): bind DPI PFs to vfio-pci by default; enable --num_vfs
#   VFs in order across all DPI PFs (fill PF0, then PF1, ...).
#
# If called with 'unbind', undo the steps (rebind default drivers, remove VFs,
# cleanup hugepages).

set -euo pipefail

# ----------------------------- Config ---------------------------------
NUM_DPI=1                   # CN10K: number of DPI PFs to use
NUMVFS=12                   # CN10K: VFs to create per DPI PF
NUM_VFS_CN20K=0             # CN20K: total VFs to enable across all DPI PFs
HUGEPG_SZ_KB=524288         # 512MB hugepages (524288 kB)
HUGEPG_COUNT=12             # Number of 512MB hugepages to allocate (fixed)
HUGEPG_MNT=/dev/huge        # HugeTLB mountpoint for DPDK

CN10K_DPI_PF_ID="177d:a080"
CN10K_DPI_VF_ID="177d:a081"
CN20K_DPI_PF_ID="177d:a0e8"
CN20K_DPI_VF_ID="177d:a0e9"
NPA_PF_ID="177d:a0fb"

# State file to record original drivers (optional; restore also works without it)
STATE_FILE="/var/run/dpi_vfio_bind.state"

ACTION="bind"

# ----------------------------- Helpers --------------------------------
msg() { echo -e "[*] $*"; }
err() { echo -e "[!] $*" >&2; }

usage() {
  cat <<EOF
Usage: $0 [bind|unbind] [options]

CN10K (177d:a080/a081) — unchanged; do not use --num_vfs:
  --num_dpi=N       Number of DPI PFs to use (default: $NUM_DPI)
  --vfs_per_pf=N    VFs per PF via SRIOV (default: $NUMVFS)

CN20K (177d:a0e8/a0e9) only:
  --num_vfs=N       Total VFs to enable across all DPI PFs, assigned in PF
                    order (e.g. 4 PFs x 4 VFs each, --num_vfs=10 enables
                    all VFs on PF0 and PF1 and 2 VFs on PF2). PFs are bound
                    to vfio-pci by default (default num_vfs: $NUM_VFS_CN20K).

Common:
  NPA PF (177d:a0fb) is bound to vfio-pci when present.
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
    bind|unbind)
      ACTION="$1"
      shift
      ;;
    --num_dpi=*)
      NUM_DPI="${1#*=}"
      shift
      ;;
    --num_dpi)
      NUM_DPI="$2"
      shift 2
      ;;
    --vfs_per_pf=*)
      NUMVFS="${1#*=}"
      shift
      ;;
    --vfs_per_pf)
      NUMVFS="$2"
      shift 2
      ;;
    --num_vfs=*)
      NUM_VFS_CN20K="${1#*=}"
      shift
      ;;
    --num_vfs)
      NUM_VFS_CN20K="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      err "Unknown argument: $1"
      usage
      exit 2
      ;;
    esac
  done
}

require_root() {
  if [[ $EUID -ne 0 ]]; then
    err "This script must be run as root."
    exit 1
  fi
}

mounted_hugetlbfs() {
  mountpoint -q "$HUGEPG_MNT" && grep -q " $HUGEPG_MNT " /proc/mounts | grep -q hugetlbfs
}

get_current_driver() {
  local bdf="$1"
  if [[ -L "/sys/bus/pci/devices/$bdf/driver" ]]; then
    basename "$(readlink -f "/sys/bus/pci/devices/$bdf/driver")"
  else
    echo "none"
  fi
}

bind_to_vfio() {
  local bdf="$1"
  local curdrv
  curdrv="$(get_current_driver "$bdf")"

  # Save original driver mapping
  echo "$bdf,$curdrv" >> "$STATE_FILE"

  if [[ "$curdrv" != "vfio-pci" && -e "/sys/bus/pci/devices/$bdf/driver/unbind" ]]; then
    echo "$bdf" > "/sys/bus/pci/devices/$bdf/driver/unbind" || true
  fi

  echo vfio-pci > "/sys/bus/pci/devices/$bdf/driver_override"
  echo "$bdf" > /sys/bus/pci/drivers_probe

  local nowdrv
  nowdrv="$(get_current_driver "$bdf")"
  if [[ "$nowdrv" != "vfio-pci" ]]; then
    err "Failed to move $bdf to vfio-pci (now: $nowdrv)"
    exit 1
  fi
  msg "Device $bdf moved to vfio-pci (was: $curdrv)"
}

restore_from_vfio() {
  local bdf="$1"

  # Clear override first so default matching can occur
  if [[ -w "/sys/bus/pci/devices/$bdf/driver_override" ]]; then
    : > "/sys/bus/pci/devices/$bdf/driver_override" || true
  fi

  # If currently bound to vfio-pci, unbind
  if [[ -e "/sys/bus/pci/drivers/vfio-pci/unbind" ]]; then
    if [[ "$(get_current_driver "$bdf")" == "vfio-pci" ]]; then
      echo "$bdf" > /sys/bus/pci/drivers/vfio-pci/unbind || true
    fi
  fi

  # Re-probe so the default kernel driver (matching the PCI ID) attaches
  echo "$bdf" > /sys/bus/pci/drivers_probe || true

  msg "Device $bdf restored to kernel driver: $(get_current_driver "$bdf")"
}

# ----------------------------- Discovery ------------------------------
get_pfs_by_pci_id() {
  local pci_id="$1"
  local limit="${2:-0}"

  if [[ "$limit" -gt 0 ]]; then
    lspci -Dnnd "$pci_id" | awk '{print $1}' | head -"$limit"
  else
    lspci -Dnnd "$pci_id" | awk '{print $1}'
  fi
}

get_vfs_by_pci_id() {
  local pci_id="$1"
  lspci -Dnnd "$pci_id" | awk '{print $1}'
}

get_npapf() {
  lspci -Dnnd "$NPA_PF_ID" | awk '{print $1}' | head -1
}

# ----------------------------- Actions --------------------------------
hugepages_setup() {
  msg "Mounting hugetlbfs and allocating hugepages..."
  mkdir -p "$HUGEPG_MNT"
  if ! mounted_hugetlbfs; then
    mount -t hugetlbfs nodev "$HUGEPG_MNT"
  fi
  local hp_sys="/sys/kernel/mm/hugepages/hugepages-${HUGEPG_SZ_KB}kB/nr_hugepages"
  if [[ -w "$hp_sys" ]]; then
    echo "$HUGEPG_COUNT" > "$hp_sys"
    msg "Set ${HUGEPG_COUNT} hugepages of size ${HUGEPG_SZ_KB}kB"
  else
    err "Hugepage size ${HUGEPG_SZ_KB}kB not supported on this kernel."
    exit 1
  fi
}

hugepages_teardown() {
  msg "Releasing hugepages and unmounting hugetlbfs..."
  local hp_sys="/sys/kernel/mm/hugepages/hugepages-${HUGEPG_SZ_KB}kB/nr_hugepages"
  if [[ -w "$hp_sys" ]]; then
    echo 0 > "$hp_sys" || true
  fi
  if mounted_hugetlbfs; then
    umount "$HUGEPG_MNT" || true
  fi
}

create_dpi_vfs_per_pf() {
  local pf_list="$1"
  local vfs_per_pf="$2"
  local pf
  for pf in $pf_list; do
    local cur="$(cat /sys/bus/pci/devices/$pf/sriov_numvfs)"
    msg "Current number of VFs under DPI PF $pf = $cur"
    if [[ "$cur" != "$vfs_per_pf" ]]; then
      local total="$(cat /sys/bus/pci/devices/$pf/sriov_totalvfs)"
      local want="$vfs_per_pf"
      if (( total < vfs_per_pf )); then
        want="$total"
      fi
      msg "Creating $want VFs for DPI PF $pf ..."
      echo 0 > "/sys/bus/pci/devices/$pf/sriov_numvfs"
      echo "$want" > "/sys/bus/pci/devices/$pf/sriov_numvfs"
      if [[ $? -ne 0 ]]; then
        err "Failed to enable VFs for $pf"
        exit 1
      fi
    fi
  done
}

# CN20K: enable NUM_VFS_CN20K VFs across all PFs (fill PF0, PF1, ...).
create_cn20k_dpi_vfs() {
  local pf_list="$1"
  local num_vfs_wanted="$2"
  local remaining="$num_vfs_wanted"
  local pf

  if [[ "$num_vfs_wanted" -le 0 ]]; then
    for pf in $pf_list; do
      if [[ -w "/sys/bus/pci/devices/$pf/sriov_numvfs" ]]; then
        echo 0 > "/sys/bus/pci/devices/$pf/sriov_numvfs" || true
      fi
    done
    msg "CN20K: num_vfs=0, no DPI VFs enabled"
    return 0
  fi

  msg "CN20K: enabling $num_vfs_wanted DPI VFs across PFs ..."
  for pf in $pf_list; do
    local total want

    total="$(cat /sys/bus/pci/devices/$pf/sriov_totalvfs)"
    if [[ "$remaining" -le 0 ]]; then
      want=0
    elif (( remaining >= total )); then
      want="$total"
    else
      want="$remaining"
    fi

    msg "DPI PF $pf: enabling $want VFs (total available $total)"
    echo 0 > "/sys/bus/pci/devices/$pf/sriov_numvfs"
    echo "$want" > "/sys/bus/pci/devices/$pf/sriov_numvfs"
    if [[ $? -ne 0 ]]; then
      err "Failed to enable $want VFs for $pf"
      exit 1
    fi
    remaining=$((remaining - want))
  done

  if [[ "$remaining" -gt 0 ]]; then
    err "Requested $num_vfs_wanted VFs but only $((num_vfs_wanted - remaining)) could be enabled"
    exit 1
  fi
}

destroy_dpi_vfs() {
  local pf_list="$1"
  local pf
  for pf in $pf_list; do
    if [[ -w "/sys/bus/pci/devices/$pf/sriov_numvfs" ]]; then
      msg "Removing VFs for DPI PF $pf ..."
      echo 0 > "/sys/bus/pci/devices/$pf/sriov_numvfs" || true
    fi
  done
}

bind_devices_cn10k() {
  local dpivf_list="$1"
  local npapf="$2"

  : > "$STATE_FILE"
  msg "###### CN10K DPI VFs ######"
  echo "$dpivf_list"
  msg "Using NPA PF $npapf ..."
  local dev
  for dev in $dpivf_list $npapf; do
    [[ -z "$dev" ]] && continue
    bind_to_vfio "$dev"
  done
  msg "Bindings recorded in $STATE_FILE"
}

bind_devices_cn20k() {
  local dpipf_list="$1"
  local dpivf_list="$2"
  local npapf="$3"

  : > "$STATE_FILE"
  msg "###### CN20K DPI PFs (vfio-pci) ######"
  echo "$dpipf_list"
  local dev
  for dev in $dpipf_list; do
    [[ -z "$dev" ]] && continue
    bind_to_vfio "$dev"
  done

  if [[ -n "$dpivf_list" ]]; then
    msg "###### CN20K DPI VFs (vfio-pci) ######"
    echo "$dpivf_list"
    for dev in $dpivf_list; do
      bind_to_vfio "$dev"
    done
  else
    msg "CN20K: no DPI VFs to bind"
  fi

  if [[ -n "$npapf" ]]; then
    msg "Using NPA PF $npapf ..."
    bind_to_vfio "$npapf"
  fi
  msg "Bindings recorded in $STATE_FILE"
}

restore_devices() {
  local dev_list="$1"

  local dev
  for dev in $dev_list; do
    [[ -z "$dev" ]] && continue
    restore_from_vfio "$dev"
  done

  if [[ -s "$STATE_FILE" ]]; then
    msg "Restoring any remaining devices from state file..."
    while IFS=, read -r bdf _; do
      [[ -z "$bdf" ]] && continue
      if [[ -e "/sys/bus/pci/devices/$bdf" ]]; then
        restore_from_vfio "$bdf"
      fi
    done < "$STATE_FILE"
    rm -f "$STATE_FILE" || true
  fi
}

cn10k_bind() {
  local dpipf="$1"
  local npapf

  if [[ "$NUM_VFS_CN20K" -ne 0 ]]; then
    msg "CN10K: --num_vfs is not used (ignored); use --vfs_per_pf if needed"
  fi

  msg "CN10K DPI setup (PF stays on kernel driver)"
  create_dpi_vfs_per_pf "$dpipf" "$NUMVFS"

  local dpivf
  dpivf="$(get_vfs_by_pci_id "$CN10K_DPI_VF_ID")"
  echo
  msg "###### DPI VFs ######"
  [[ -n "$dpivf" ]] && echo "$dpivf" || err "No CN10K DPI VFs found after creation"

  npapf="$(get_npapf)"
  echo
  bind_devices_cn10k "$dpivf" "$npapf"
}

cn10k_unbind() {
  local dpipf="$1"
  local dpivf npapf devs

  dpivf="$(get_vfs_by_pci_id "$CN10K_DPI_VF_ID")"
  npapf="$(get_npapf)"
  devs="$dpivf"
  [[ -n "$npapf" ]] && devs="$devs $npapf"

  restore_devices "$devs"
  destroy_dpi_vfs "$dpipf"
}

cn20k_bind() {
  local dpipf="$1"
  local npapf dpivf

  msg "CN20K DPI setup (PFs bound to vfio-pci)"
  create_cn20k_dpi_vfs "$dpipf" "$NUM_VFS_CN20K"

  dpivf="$(get_vfs_by_pci_id "$CN20K_DPI_VF_ID")"
  echo
  if [[ "$NUM_VFS_CN20K" -gt 0 ]]; then
    msg "###### DPI VFs ######"
    [[ -n "$dpivf" ]] && echo "$dpivf" || err "No CN20K DPI VFs found after creation"
  fi

  npapf="$(get_npapf)"
  echo
  bind_devices_cn20k "$dpipf" "$dpivf" "$npapf"
}

cn20k_unbind() {
  local dpipf="$1"
  local dpivf npapf devs

  dpivf="$(get_vfs_by_pci_id "$CN20K_DPI_VF_ID")"
  npapf="$(get_npapf)"
  devs="$dpivf $dpipf"
  [[ -n "$npapf" ]] && devs="$devs $npapf"

  restore_devices "$devs"
  destroy_dpi_vfs "$dpipf"
}

# ----------------------------- Main -----------------------------------
require_root
parse_args "$@"

CN20K_DPIPF="$(get_pfs_by_pci_id "$CN20K_DPI_PF_ID")"
CN10K_DPIPF="$(get_pfs_by_pci_id "$CN10K_DPI_PF_ID" "$NUM_DPI")"

if [[ -n "$CN20K_DPIPF" && -n "$CN10K_DPIPF" ]]; then
  err "Both CN10K and CN20K DPI PFs present; run on a single platform type"
  exit 1
fi

if [[ -z "$CN20K_DPIPF" && -z "$CN10K_DPIPF" ]]; then
  err "No DPI PFs found (CN10K $CN10K_DPI_PF_ID or CN20K $CN20K_DPI_PF_ID)"
  exit 1
fi

case "$ACTION" in
bind)
  hugepages_setup
  echo
  if [[ -n "$CN20K_DPIPF" ]]; then
    msg "###### CN20K DPI PFs ######"
    echo "$CN20K_DPIPF"
    cn20k_bind "$CN20K_DPIPF"
  else
    msg "###### CN10K DPI PFs ######"
    echo "$CN10K_DPIPF"
    cn10k_bind "$CN10K_DPIPF"
  fi
  ;;

unbind)
  if [[ -n "$CN20K_DPIPF" ]]; then
    cn20k_unbind "$CN20K_DPIPF"
  else
    cn10k_unbind "$CN10K_DPIPF"
  fi
  hugepages_teardown
  msg "Unbind and cleanup complete."
  ;;

*)
  err "Unknown action: $ACTION"
  usage
  exit 2
  ;;

esac
