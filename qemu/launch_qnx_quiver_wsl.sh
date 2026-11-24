#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
project_root="$(cd -- "$script_dir/.." && pwd)"
qemu_bin="$(printenv QEMU_BIN || printf 'qemu-system-x86_64')"
vm_root="$(printenv QNX_VM_ROOT || printf '/mnt/c/Users/Lenovo/qnxvm')"
disk_image="$(printenv QNX_DISK_IMAGE || printf '%s/output/disk-qemu.vmdk' "$vm_root")"
ifs_image="$(printenv QNX_IFS_IMAGE || printf '%s/output/ifs.bin' "$vm_root")"
shm_path="$(printenv QNX_QUIVER_SHM || printf '%s/build/qnx_quiver_shm_v3.bin' "$project_root")"
host_ssh_port="$(printenv QNX_SSH_PORT || printf '2223')"
qmp_port="$(printenv QNX_QMP_PORT || printf '4443')"
monitor_port="$(printenv QNX_MONITOR_PORT || printf '4442')"
serial_backend="$(printenv QNX_SERIAL_BACKEND || printf 'file:%s/build/qemu_serial.log' "$project_root")"
qemu_accel="$(printenv QEMU_ACCEL || printf 'tcg')"
headless="$(printenv QUIVER_HEADLESS || printf '0')"
pci_topology="$(printenv QNX_PCI_TOPOLOGY || printf 'legacy')"

machine_args=()
ivshmem_args=(-device "ivshmem-plain,memdev=qnx_shm,id=quiver_ivshmem")
case "$pci_topology" in
    legacy)
        ;;
    pci-bridge-hotplug)
        # Keep the image's working i440fx/EIDE machine and put ivshmem behind
        # an ACPI-hotpluggable PCI bridge so QNX can receive slot events.
        ivshmem_args=(
            -device pci-bridge,id=quiver_bridge,chassis_nr=1
            -device "ivshmem-plain,memdev=qnx_shm,bus=quiver_bridge,id=quiver_ivshmem"
        )
        ;;
    *)
        printf 'Unsupported QNX_PCI_TOPOLOGY=%s (use legacy or pci-bridge-hotplug).\n' "$pci_topology" >&2
        exit 2
        ;;
esac

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
    exec "$qemu_bin" "${machine_args[@]}" -smp 2 -cpu max -m 1G -snapshot \
        -drive "file=$disk_image,if=ide,id=drv0" -kernel "$ifs_image" \
        -object "memory-backend-file,size=4M,share=on,mem-path=$shm_path,id=qnx_shm" \
        "${ivshmem_args[@]}" \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$host_ssh_port-:22" \
        -device virtio-net-pci,netdev=net0 -device virtio-rng-pci \
        -qmp "tcp:127.0.0.1:$qmp_port,server=on,wait=off" \
        -accel "$qemu_accel" -display none \
        -serial "$serial_backend" \
        -monitor "tcp:127.0.0.1:$monitor_port,server=on,wait=off"
else
    exec "$qemu_bin" "${machine_args[@]}" -smp 2 -cpu max -m 1G -snapshot \
        -drive "file=$disk_image,if=ide,id=drv0" -kernel "$ifs_image" \
        -object "memory-backend-file,size=4M,share=on,mem-path=$shm_path,id=qnx_shm" \
        "${ivshmem_args[@]}" \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$host_ssh_port-:22" \
        -device virtio-net-pci,netdev=net0 -device virtio-rng-pci \
        -qmp "tcp:127.0.0.1:$qmp_port,server=on,wait=off" \
        -accel "$qemu_accel" -nographic -serial stdio -monitor none
fi
