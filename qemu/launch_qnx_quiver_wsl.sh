#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
project_root="$(cd -- "$script_dir/.." && pwd)"
qemu_bin="$(printenv QEMU_BIN || printf 'qemu-system-x86_64')"
vm_root="$(printenv QNX_VM_ROOT || printf '/mnt/c/Users/Lenovo/qnxvm')"
disk_image="$(printenv QNX_DISK_IMAGE || printf '%s/output/disk-qemu.vmdk' "$vm_root")"
ifs_image="$(printenv QNX_IFS_IMAGE || printf '%s/output/ifs.bin' "$vm_root")"
shm_path="$(printenv QNX_QUIVER_SHM || printf '%s/build/qnx_quiver_shm.bin' "$project_root")"
host_ssh_port="$(printenv QNX_SSH_PORT || printf '2223')"
qemu_accel="$(printenv QEMU_ACCEL || printf 'tcg')"
headless="$(printenv QUIVER_HEADLESS || printf '0')"

for required in "$disk_image" "$ifs_image"; do
    if [[ ! -f "$required" ]]; then
        printf 'Required QNX VM file is missing: %s\n' "$required" >&2
        exit 2
    fi
done

if ! "$qemu_bin" -device help 2>&1 | grep -q 'ivshmem-plain'; then
    printf 'QNX-Quiver requires an ivshmem-plain capable QEMU build.\n' >&2
    exit 2
fi

mkdir -p "$(dirname -- "$shm_path")"
if [[ ! -e "$shm_path" ]]; then
    truncate -s 4M "$shm_path"
elif [[ "$(stat -c '%s' "$shm_path")" != 4194304 ]]; then
    printf 'Shared-memory file must be exactly 4 MiB: %s\n' "$shm_path" >&2
    exit 2
fi

printf 'Starting QNX-Quiver with %s\n' "$qemu_bin"
printf 'BAR2 backing file: %s\n' "$shm_path"
printf 'Guest SSH forwarding: 127.0.0.1:%s -> guest:22\n' "$host_ssh_port"

if [[ "$headless" == 1 ]]; then
    exec "$qemu_bin" -smp 2 -cpu max -m 1G -snapshot \
        -drive "file=$disk_image,if=ide,id=drv0" -kernel "$ifs_image" \
        -object "memory-backend-file,size=4M,share=on,mem-path=$shm_path,id=qnx_shm" \
        -device ivshmem-plain,memdev=qnx_shm,id=quiver_ivshmem \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$host_ssh_port-:22" \
        -device virtio-net-pci,netdev=net0 -device virtio-rng-pci \
        -qmp tcp:127.0.0.1:4443,server=on,wait=off \
        -accel "$qemu_accel" -display none \
        -serial "file:$project_root/build/qemu_serial.log" \
        -monitor tcp:127.0.0.1:4442,server=on,wait=off
else
    exec "$qemu_bin" -smp 2 -cpu max -m 1G -snapshot \
        -drive "file=$disk_image,if=ide,id=drv0" -kernel "$ifs_image" \
        -object "memory-backend-file,size=4M,share=on,mem-path=$shm_path,id=qnx_shm" \
        -device ivshmem-plain,memdev=qnx_shm,id=quiver_ivshmem \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$host_ssh_port-:22" \
        -device virtio-net-pci,netdev=net0 -device virtio-rng-pci \
        -qmp tcp:127.0.0.1:4443,server=on,wait=off \
        -accel "$qemu_accel" -nographic -serial stdio -monitor none
fi
