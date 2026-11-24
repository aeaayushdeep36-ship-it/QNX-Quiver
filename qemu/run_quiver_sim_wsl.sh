#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
project_root="$(cd -- "$script_dir/.." && pwd)"
shm_path="${QNX_QUIVER_SHM:-$project_root/build/qnx_quiver_shm.bin}"
exec python3 -B "$project_root/host/sim_engine/quiver_sim.py" --shm "$shm_path" "$@"
