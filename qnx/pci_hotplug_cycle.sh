#!/bin/sh
set -eu

timeout="${1:-45}"
case "$timeout" in *[!0-9]*|'') echo "timeout must be an integer" >&2; exit 2 ;; esac

/proc/boot/pci_hotplug_wait.sh removed "$timeout"
/proc/boot/pci_hotplug_wait.sh reinserted "$timeout"
