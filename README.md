# QNX-Quiver

QNX-Quiver is a QNX SDP 8.0 x86_64 virtual embedded platform. A host plant simulator exchanges CAN, IMU, and actuator records with QNX services through an ivshmem BAR2 protocol. QNX clients access shared records through `/dev/quiver`; the PCI manager retains sole ownership of the PCI function and mapping.

## Current implementation

The project includes protocol v3, a resident PCI resource manager with PCI-server restart recovery, a QNX client API, split libcan CAN-FD RX/TX endpoints, IMU and actuator utilities, fault tracing, a controller demonstration, a development supervisor, and a WSL QEMU launcher. Exact build and evidence status is in [docs/implementation-status.md](docs/implementation-status.md).

## Build and host checks

From the QNX SDP 8.0 environment, run:

```sh
make clean all
```

The build uses `-Wall -Wextra -Werror` and creates the QNX binaries in `build/`. Run host protocol tests on Windows or Linux with Python 3.10+:

```sh
python -m unittest discover -s tests -v
python host/sim_engine/quiver_sim.py --self-test
```

The simulator reports measured rate, wake-lateness percentiles, and missed deadlines during bounded runs. For injected latency characterization, pass `--duration 2 --latency-spike-us 500`. Python/Windows/WSL timing is observational and is not a hard real-time guarantee.

## QEMU and shared memory

The Windows QEMU 11.1.0 installation lacks `ivshmem-plain`; the supported live path is Ubuntu WSL QEMU 10.2.1. Run both QEMU and the simulator inside WSL against the same v3 backing file:

```sh
bash qemu/launch_qnx_quiver_wsl.sh
bash qemu/run_quiver_sim_wsl.sh
```

For a software PCI lifecycle test, launch the guest with `QNX_PCI_TOPOLOGY=pci-bridge-hotplug`. Load both QNX PCI event-handler and hotplug bus-configuration modules using [qnx/pci_server_hotplug.cfg](qnx/pci_server_hotplug.cfg), then follow the remove/reinsert steps in [qnx/README.md](qnx/README.md). A successful QMP `device_del` reply is not enough: the test requires `DEVICE_DELETED`, inventory change, `/dev/quiver` withdrawal, and reacquisition. The current i440fx/PCI-bridge test still does not complete removal even with those modules active; software hotplug remains open pending a QEMU topology that QNX supports.

## QNX guest flow

After `pci-server` is running, start `build/quiver-pci-rm`. Start CAN RX and TX separately because the supplied guest rejects a second libcan path attach from one process:

```sh
build/dev-can-quiver rx &
build/dev-can-quiver tx &
build/quiver-can-probe
build/quiver-can-probe --status
```

Run one IMU-ring consumer at a time: `build/quiver-imu` or `build/quiver-controller`. `build/quiver-actuator value [ttl-ms]` sends a bounded command and waits for its command-ID acknowledgement. `build/quiver-trace [path.csv]` records fault and telemetry snapshots. `build/quiver-supervisor` coordinates development startup and restart; product use still requires distinct process types and a verified boot policy.

The security policy in `qnx/security/qnx_quiver.policy` is only a compile-checked fragment. The current guest has no active `/proc/boot/secpol.bin`, and the PCI-server channel type is image-specific; read the policy section in the QNX guide before product integration.

## Project layout

- `include/`: protocol v3 and client API v3 definitions.
- `host/sim_engine/`: plant simulator, shared-memory protocol, fault controls, and timing metrics.
- `qnx/dev-quiver/`: PCI service, resident resource manager, and client library.
- `qnx/dev-can-quiver/`: QNX CAN DDK frontend and probes.
- `qnx/services/`: IMU consumer, actuator client, trace utility, and controller demo.
- `qnx/supervisor/`: development process supervisor.
- `qemu/`, `tests/`, and `codelab/`: launchers, host/QMP tests, and clean-environment handoff.

Protocol format and ownership rules are in [docs/protocol.md](docs/protocol.md). The project name is QNX-Quiver; QVB1 and device IDs are technical protocol/platform identifiers.
