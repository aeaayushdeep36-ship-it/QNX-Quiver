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

No physical hardware is required for a virtual lifecycle test. The WSL QEMU launcher supports `QNX_PCI_TOPOLOGY=pci-bridge-hotplug`; it retains the working i440fx/EIDE machine and places ivshmem behind a PCI bridge. QNX requires a bus-configuration module for hotplug reconfiguration, so `pci_server_hotplug.cfg` enables `DO_BUS_CONFIG=yes` and selects the hotplug module as well as the event handler. Embed this file as `/proc/boot/pci_server.cfg` in a throwaway test IFS and load that config at initial PCI-server startup, before device clients start. The bus configuration can change resources; restarting PCI with it on a live working system is not the recommended test setup.

Boot the test IFS with the hotplug PCI-server config active, then transfer `pci_hotplug_wait.sh` and the rebuilt resource-manager binary to the guest using its authorized file-transfer path. Start `/tmp/quiver-pci-rm`. In another guest shell run `sh /tmp/pci_hotplug_wait.sh removed 30`. From WSL run `python3 tests/qemu_pci_hotplug.py --port 4443` to request removal and reinsert the ivshmem function. The QMP harness holds the device absent for three seconds by default so the manager's one-second health check can withdraw the node; tune with `--hold-removed-seconds`. After the harness completes, run `sh /tmp/pci_hotplug_wait.sh reinserted 30`. The QMP script requires a `DEVICE_DELETED` event and checks `query-pci`; a successful `device_del` reply alone is not treated as proof. A full pass requires both QMP inventory and QNX node withdrawal/reacquisition.

The default guest config and a throwaway IFS with matching QNX 8.0 PCI modules plus the hotplug event and bus-configuration modules both failed to remove ivshmem under the current i440fx PCI-bridge topology. The configured PCI server logged that link events were unsupported; QMP accepted `device_del`, but emitted no `DEVICE_DELETED` event and `query-pci` continued to show the device. The guest kept `/dev/quiver`. This software-only path remains unverified; try a QNX-supported QEMU PCIe hotplug topology. No physical hardware is required for that software test.

## Security boundary

`security/qnx_quiver.policy` is a compile-checked fragment, not the active boot policy. The supplied guest has no active `/proc/boot/secpol.bin`, and the exact product PCI-server channel type is image-specific. Merge the fragment into the target policy, obtain the PCI-server type from `secpolmonitor`, assign each service type at launch, then test both allowed and denied client operations with enforcement enabled before production deployment.
