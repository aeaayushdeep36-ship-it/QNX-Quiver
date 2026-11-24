# QNX-Quiver implementation status

## Completed

- Discovered the QNX SDP 8.0 toolchain, x86_64 CAN DDK library and headers, QEMU binaries, and the existing QNX VM image.
- Added the 4 MiB BAR2 protocol header, exact offset/layout assertions, 128-byte slots that can hold CAN-FD metadata and payload, and a QNX x86_64 layout-check program.
- Added a Windows/Linux host plant and fault-injection simulator with shared-memory ring operations.
- Added a Windows QEMU capability-gated launcher and a WSL QEMU launcher using file-backed ivshmem-plain.
- Added a WSL simulator wrapper so QEMU and the host plant can map the same project backing file in WSL.
- Added a QNX minimal PCI bring-up service that claims the ivshmem device, maps BAR2, validates QVB1, and releases its resources.
- Added a resident QNX resource manager that keeps exclusive PCI ownership/BAR2 mapping and exposes read-only `/dev/quiver` status.
- Added one-second PCI/BAR health checks: on PCI-server loss or invalid BAR2, the manager withdraws `/dev/quiver`, invalidates stale resources, retries, and republishes only after BAR2 and QVB1 validate.
- Added a versioned QNX client `devctl` API and wrappers for info, host-to-QNX ring pops, QNX-to-host ring pushes, fault snapshots, and updates to QNX-owned telemetry counters. BAR2 stays private to the resource manager; host fault flags/latency are read-only to QNX clients.
- Added a QNX SDP 8.0 `libcan` CAN-FD frontend for `/dev/can0/rx0` and `/dev/can0/tx0`. It maps 64-byte frames and extended IDs through QVB1 rings, validates CAN-FD DLC lengths, transfers the CRC-corrupt marker, and updates QNX-owned frame counters. Run RX and TX endpoints as separate processes because the current target returns `EPERM` on a second libcan path attach from one process.
- Added an RX polling worker and bounded frame queue; blocking raw RX waits on the queue, while nonblocking RX returns `EAGAIN` when empty.
- Added signal-safe RX shutdown coordination with libcan: SIGTERM stops the polling worker, wakes blocked RX requests with `ECANCELED`, lets the resource manager unwind, and withdraws the CAN nodes. Verified this against an empty-queue blocked client in the QNX guest.
- Added a guest-side CAN-FD probe that sends an extended-ID frame with 64 unique data bytes, checks the full host-simulator loopback, and verifies the shared counters.
- Added a QNX security-policy fragment with least-privilege CAN/PCI process types, attach paths, channel permissions, and root-only node permissions; switched the PCI manager's `/dev/quiver` attach to `secpol_resmgr_attach()`.

## Verification

- The QNX protocol layout check cross-compiles for QNX 8.0 x86_64 with warnings treated as errors.
- The host simulator full-ring/wraparound self-test passes. A bounded run measured approximately 999 Hz for a 1 kHz target.
- The existing guest boots in snapshot mode. Without ivshmem, its PCI list contains 1af4:1000 and 1af4:1005 but not 1af4:1110.
- Ubuntu WSL QEMU 10.2.1 boots the guest with `ivshmem-plain`; QNX `pci-tool` identifies 1af4:1110, BAR0 at 0x100 bytes, and BAR2 at 4 MiB.
- QNX `pci-tool` reads the QVB1 protocol header from BAR2. A WSL simulator run updated ring 2's head to 1201, and the guest read that live value from BAR2.
- The minimal service compiles with QNX SDP 8.0 x86_64 and runs in the guest: it reports BAR0 `0xfeb91000`/256 bytes, BAR2 `0xfe000000`/4 MiB, validates QVB1 v1, and exits successfully.
- The resident resource manager was started in the guest; `cat /dev/quiver` returned the expected device/BAR/protocol record, and 1-byte reads returned the correct first 16 bytes. SIGTERM removed the process and node; a fresh start reacquired the device and served the node again.
- The new QNX client API and service cross-compile with `-Wall -Wextra -Werror`. The guest client probe successfully read API/QVB1 info, ring geometry, fault flags, and counters through `devctl`.
- A guest client popped one marked host-to-QNX CAN RX record and advanced its tail; it pushed a marked QNX-to-host CAN TX record that appeared at the expected slot/head in the shared BAR backing file. The QNX CAN TX telemetry counter incremented through the API.
- CAN-FD extended-ID/64-byte loopback passed live in the QNX guest: the 64-byte payload echoed byte-for-byte through the host simulator and both QNX TX/host RX counters advanced. The protocol full-ring/wraparound and CAN-FD record self-test also passes.
- Blocking RX passed both scheduling checks in the live guest: an empty-queue reader woke on newly resumed simulator traffic, and a blocked reader returned `ECANCELED` when the service received SIGTERM; the service exited and `/dev/can0` was withdrawn.
- The QNX security-policy fragment compiles with the SDP 8.0 `secpolcompile` tool. It cannot be runtime-enforced in the current guest, which has no active `/proc/boot/secpol.bin`; the final PCI-server type/connect permission must be added for the product image.
- With the resident manager attached, terminating `pci-server` caused `PCI_ERR_SERVER_NOT_AVAILABLE`; `/dev/quiver` disappeared while the manager stayed alive. Restarting `pci-server --aspace-enable --config=/proc/boot/pci_server.cfg` restored PCI access; the manager reclaimed BAR2, validated QVB1, and republished `/dev/quiver`. SIGTERM after recovery removed the node cleanly.
- The manager compiled with QNX SDP 8.0 x86_64 using `-Wall -Wextra -Werror` after adding the polling timeout callback and ETIMEDOUT handling.
- Repeated QMP checks on QEMU 10.2.1/i440FX returned success for `device_del quiver_ivshmem`, but the device remained in `query-pci` before and after; actual PCI hot-unplug/reinsert could not be verified. Startup with the PCI server/device absent now fails with a diagnostic; restart the service after they are available.
- A controlled build without `ThreadCtl(_NTO_TCTL_IO, 0)` faults with `Memory fault`; the same service with I/O privilege succeeds. The PCI API fault investigation and old BAR config access mistake are documented in `docs/pci-service-investigation.md`.
- A Windows simulator process and WSL QEMU did not observe each other's live writes through the same DrvFS backing file. Use the WSL simulator wrapper with the WSL guest.

## Remaining gates

- Production policy enforcement is not runtime-verified: the current guest has no active `/proc/boot/secpol.bin`. Merge the fragment into the product image baseline, assign process types at launch, identify and grant the exact PCI-server channel type, then verify the services and client under enforcement.
- Actual PCI hot-unplug/reinsert remains unverified because the current QEMU i440FX topology leaves the ivshmem function present after `device_del`; use a hotplug-capable topology or hardware.
- Next implementation phase: add IMU and actuator services, then supervisor, trace path, and controller example.
