# QNX-Quiver development environment

Discovery and verification date: 25 November 2026.

## QNX SDP 8.0

- Environment script: `C:\Users\Lenovo\qnx800\qnxsdp-env.bat`.
- `QNX_HOST=C:\Users\Lenovo\qnx800\host\win64\x86_64` and `QNX_TARGET=C:\Users\Lenovo\qnx800\target\qnx`.
- The x86_64 CAN DDK headers and `libcan` are installed; `qcc -Vgcc_ntox86_64` is the supported target.
- The project currently cross-compiles all binaries and the policy fragment with warnings as errors.

## QEMU and guest

- Windows QEMU 11.1.0 does not expose `ivshmem-plain`.
- Ubuntu WSL QEMU 10.2.1 exposes `ivshmem-plain` and boots the supplied QNX guest at `C:\Users\Lenovo\qnxvm\output\ifs.bin` with its disk in snapshot mode.
- `QNX_PCI_TOPOLOGY=pci-bridge-hotplug` retains the working i440fx/EIDE topology and places ivshmem behind a PCI bridge. `query-pci` showed device `1af4:1110` on bus 1 with BAR0 256 bytes and BAR2 4 MiB.
- In the default guest configuration, QMP `device_del` returned but did not complete removal: no `DEVICE_DELETED` event arrived and the device remained in `query-pci`. The test IFS loaded the matching QNX 8.0 PCI server, libpci, hardware module, capability modules, event handler, and hotplug bus-configuration module. PCI server startup completed, but logs reported link events unsupported; another `device_del` still produced no event and left ivshmem in `query-pci`. Removal/reinsert remains unverified on this i440fx topology.
- The SSH forwarding port defaults to 2223 and QMP defaults to 4443; both are configurable in the WSL launcher.

## Shared-memory boundary

For live updates, run both QEMU and the host simulator inside WSL against `build/qnx_quiver_shm_v3.bin`. Windows and WSL mappings through the same DrvFS file did not show each other's live writes. The simulator checks the exact file size and protocol version and refuses to overwrite an unknown nonzero image.

## Validation boundary

Host protocol tests and the QNX cross-build pass. The v3 manager, CAN-FD, IMU, actuator, trace, controller, and supervisor flows have disposable guest evidence. Supervisor startup, child restart, coordinated shutdown, PCI-manager-process outage recovery, PCI-server restart recovery on the tested Q35 bridge setup, and two fresh cold boots passed. A prior i440fx PCI-server restart did trigger a QEMU 10.2.1 assertion. Native hot-unplug/reinsert remains unverified: the QNX event path did not update its device database on the tested topology; restarting `pci-server` with ivshmem present assigned its BARs and Quiver recovered. The supplied guest has no active `/proc/boot/secpol.bin`, so product policy enforcement remains unverified. No physical CAN transceiver or PCI device is available; QEMU software tests cannot establish electrical behavior.
