#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 removed|reinserted [timeout-seconds]" >&2
    exit 2
fi
timeout="${2:-30}"
case "$1" in
    removed) expected=absent ;;
    reinserted) expected=present ;;
    *) echo "state must be removed or reinserted" >&2; exit 2 ;;
esac
case "$timeout" in *[!0-9]*|'') echo "timeout must be an integer" >&2; exit 2 ;; esac

elapsed=0
seen_initial_state=0
while [ "$elapsed" -le "$timeout" ]; do
    if [ "$expected" = present ]; then
        if [ ! -e /dev/quiver ]; then
            seen_initial_state=1
        elif [ "$seen_initial_state" -eq 1 ]; then
            echo "PASS: /dev/quiver returned after PCI device reinsertion"
            exit 0
        fi
    else
        if [ -e /dev/quiver ]; then
            seen_initial_state=1
        elif [ "$seen_initial_state" -eq 1 ]; then
            echo "PASS: /dev/quiver was withdrawn after PCI device removal"
            exit 0
        fi
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done
echo "FAIL: /dev/quiver did not become $expected within ${timeout}s" >&2
exit 1
