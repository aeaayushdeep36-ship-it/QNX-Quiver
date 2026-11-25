# QNX target components

The project targets QNX SDP 8.0 x86_64 and uses `/dev/quiver` as its only BAR2 access path. CAN, IMU, actuator, controller, and trace programs use the restricted client API in `include/quiver_client.h`.

## Build

From a QNX SDP 8.0 command prompt, set `QNX_HOST` and `QNX_TARGET`, then run `make all`. The build uses `-Wall -Wextra -Werror`; it produces the resident PCI resource manager, minimal bring-up tool, client probes, CAN DDK frontend, IMU consumer, actuator client, trace capture, controller example, and development supervisor.

The QNX guest and host simulator must use the same protocol version. Current BAR2 is protocol v3 and client API v3. Use a fresh `qnx_quiver_shm_v3.bin` backing file when switching from older protocol files; the simulator deliberately refuses to overwrite a nonzero unknown version.

## Start the PCI and CAN services

In the guest, start `build/quiver-pci-rm` after `pci-server` is available. It owns the PCI function and BAR2 mapping and publishes `/dev/quiver` only after validating QVB1 v3. Then run:

```sh
build/dev-can-quiver rx &
build/dev-can-quiver tx &
build/quiver-can-probe
build/quiver-can-probe --status
```

RX and TX remain separate processes because the supplied image rejects a second libcan path attach in one process. RX uses one serialized consumer queue; if it fills, the oldest pending item is discarded and the shared QNX CAN drop counter advances. TX retries a full ring for up to 5 ms and then reports a dropped frame. Neither path replays a request after an ambiguous device disconnect.

The CAN status devctl reports bus-off state, fault flags, and a state generation that increments on each active/off transition. `--status` reads the state. The CAN loopback is a virtual simulator path; it does not validate real arbitration, electrical timing, or a transceiver.

## Peripheral examples

- `build/quiver-imu [--stale-ms 100] [--duration seconds]` consumes the IMU ring, validates six finite floats, prints sequence and source timestamp, and reports when no fresh sample has arrived. It is a ring consumer, so do not run it at the same time as the controller example.
- `build/quiver-actuator value [ttl-ms]` validates a normalized command in `[-1,1]`, publishes it, and waits for a simulator acknowledgement keyed by command ID. The host applies a command only for its bounded TTL, then returns output to zero.
- `build/quiver-trace [output.csv] [--duration seconds] [--records N]` captures fault snapshots and telemetry counters without consuming data rings. It defaults to `/tmp/quiver-trace.csv` and stops after 10,000 rows unless a different limit is supplied.
- `build/quiver-controller` is a demonstration proportional controller. It consumes the IMU ring, bounds its output, and sends zero after a 100 ms sample gap. It competes for the IMU ring with `quiver-imu`, so run one consumer at a time.
- `build/quiver-supervisor [binary-directory]` starts the PCI manager, two CAN processes, IMU consumer, and trace capture in dependency order. It applies bounded restart backoff and terminates children on SIGINT/SIGTERM. This development supervisor inherits one process type across children; use explicit product-image type transitions for production.

## Software PCI hotplug

No physical hardware is required for virtual hotplug testing. The current test IFS builder enables the QNX PCI event-handler and hotplug bus-configuration modules. In the controlled PCI-only Q35 + PCIe-to-PCI-bridge experiment on QNX SDP 8.0 + QEMU 10.2.1, QNX logged that the bridge does not support link-event notifications. After QMP insertion, QEMU listed ivshmem but QNX did not add it to `pci-tool` or its PCI database, and both BARs remained unassigned. Restarting `pci-server` with the function present caused QNX to discover it and assign BAR0 and BAR2. The resident manager observes the changed PCI-server node identity, withdraws `/dev/quiver`, and re-executes itself to refresh libpci state; it then republishes `/dev/quiver`, and the client probe passes. Thus PCI-server restart recovery is verified, while native guest-visible hot-unplug and a full live remove/reinsert cycle remain unverified.

Host-acknowledged QEMU removal can remove the function, but it does not prove QNX received or initiated the eject. The QMP test reports an incomplete cycle as `UNVERIFIED`. Use a fresh memory-backend object ID on every reinsertion while pointing it at the same shared-memory file; reusing an attached ivshmem backend makes QEMU 10.2.1 assert. The WSL launcher now exposes `QNX_PCI_TOPOLOGY=pcie-root-port` with configurable `QNX_PCIE_ROOTPORT_CHASSIS` and `QNX_PCIE_ROOTPORT_SLOT` for another disposable experiment. That topology has not passed guest validation; previous root-port layouts faulted the supplied QNX PCI server during startup. The bridge topology still logs unsupported link events. The software-only test does not require physical hardware.

## Security boundary

`security/qnx_quiver.policy` is a compile-checked fragment, not the active boot policy. The supplied guest has no active `/proc/boot/secpol.bin`, and the exact product PCI-server channel type is image-specific. Merge the fragment into the target policy, obtain the PCI-server type from `secpolmonitor`, assign each service type at launch, then test both allowed and denied client operations with enforcement enabled before production deployment. The policy test IFS workflow and its remaining full-image integration requirement are documented in [`security/README.md`](security/README.md).
