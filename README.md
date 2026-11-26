# QNX-Quiver

QNX-Quiver is a QNX SDP 8.0 x86_64 virtual embedded platform. A host plant simulator exchanges CAN, IMU, and actuator records with QNX services through an ivshmem BAR2 protocol. QNX clients access shared records through `/dev/quiver`; the PCI manager retains sole ownership of the PCI function and mapping.

## Current implementation

The project includes protocol v3, a resident PCI resource manager with PCI-server restart recovery, a QNX client API, split libcan CAN-FD RX/TX endpoints, IMU and actuator utilities, fault tracing, a controller demonstration, a development supervisor, and a WSL QEMU launcher. Exact build and evidence status is in [docs/implementation-status.md](docs/implementation-status.md).

## Build and host checks

- **QNX Version**: QNX SDP 8.0 (`qnx800`)
- **Target Architecture**: QEMU `x86_64` (`pc` / `i440fx` and `q35`)
- **Toolchain**: `qcc -Vgcc_ntox86_64`, GNU `Make`, QNX CAN DDK (`-lcan`), QNX PCI (`-lpci`), QNX Security Policy (`-lsecpol`, `secpolcompile`), `slog2` (`-lslog2`)

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

Build a disposable test IFS with the current v3 client and service binaries by running `qemu/build_hotplug_test_ifs.ps1 -IncludeServices`. Use `-SupervisorMode` for a separate image where the supervisor owns PCI-manager startup. The guest uses a snapshot disk. For software PCI lifecycle tests, the QMP script needs the WSL-visible shared file path; see [tests/README.md](tests/README.md) for the command. A controlled PCI-only experiment found that this QNX/QEMU topology does not deliver a supported live link event for the inserted ivshmem device: QNX does not add it to its PCI database or assign BARs until `pci-server` is restarted. On server restart the manager withdraws its node and re-executes itself to refresh libpci state, then republishes `/dev/quiver`; the client probe passes. Native guest-visible hot-unplug and a complete live remove/reinsert cycle remain unverified; QMP enumeration alone is not a pass.

## QNX guest flow

After `pci-server` is running, start `build/quiver-pci-rm`. Start CAN RX and TX separately because the supplied guest rejects a second libcan path attach from one process:

```sh
build/dev-can-quiver rx &
build/dev-can-quiver tx &
build/quiver-can-probe
build/quiver-can-probe --status
```

Run one IMU-ring consumer at a time: `build/quiver-imu` or `build/quiver-controller`. `build/quiver-actuator value [ttl-ms]` sends a bounded command and waits for its command-ID acknowledgement. `build/quiver-trace [path.csv]` records fault and telemetry snapshots. `build/quiver-supervisor` coordinates startup and restart and uses `secpol_posix_spawnattr_settypeid()` to spawn each child (`quiver_pci_t`, `quiver_can_t`, `quiver_imu_t`, `quiver_trace_t`) under its own least-privilege type when a security policy is active.

`qnx/security/qnx_quiver.policy` is the Quiver integration fragment and `qnx/security/qemu_target_base.policy` is a `secpolgenerate`-derived target policy for the disposable QEMU guest. Immutable policy enforcement (`secpol -Q` -> `immutable`) has been verified in QEMU 10.2.1 snapshot mode (`pci-bridge-hotplug`) for both direct service launch (`build/ifs-quiver-policy-test.bin`, SHA256 `7D2BEC7FEA1D22309999DBA16E077F8AAAC8331A77F90C91F49AD3896B7A510F`) and typed supervisor launch (`build/ifs-quiver-supervisor-policy.bin`, SHA256 `D9AE8430C875566BC6EA8E71D13079DC63D3B74153746F4083390EF70CCE1DF3`), including `quiver_actuator_t` live acknowledgement and `quiver_denied_client_t` rejection (`EPERM`). For a production target image, replace `qemu_target_base.policy` with the target's complete active policy and type all remaining OS services; see [qnx/security/README.md](qnx/security/README.md).

## Project layout

- `include/`: protocol v3 and client API v3 definitions.
- `host/sim_engine/`: plant simulator, shared-memory protocol, fault controls, and timing metrics.
- `qnx/dev-quiver/`: PCI service, resident resource manager, and client library.
- `qnx/dev-can-quiver/`: QNX CAN DDK frontend and probes.
- `qnx/services/`: IMU consumer, actuator client, trace utility, and controller demo.
- `qnx/supervisor/`: development process supervisor.
- `qemu/`, `tests/`, and `codelab/`: launchers, host/QMP tests, and clean-environment handoff.

Protocol format and ownership rules are in [docs/protocol.md](docs/protocol.md). The project name is QNX-Quiver; QVB1 and device IDs are technical protocol/platform identifiers.

## Evidence of QNX Use & QNX-Specific Technologies

QNX-Quiver is built natively for **QNX SDP 8.0 (`x86_64`)** and uses QNX-specific microkernel, driver, and security primitives throughout:

| QNX Technology | Implementation & Exact Files | APIs / Buildflags |
|---|---|---|
| **QNX Resource Manager** | [qnx/dev-quiver/pci_rm.c](qnx/dev-quiver/pci_rm.c) | `dispatch_create_channel()`, `secpol_resmgr_attach()` publishing `/dev/quiver`, `io_devctl` handler, `ThreadCtl(_NTO_TCTL_IO, 0)`, `mmap_device_memory()` |
| **QNX Microkernel Pulses & Timers** | [qnx/dev-quiver/pci_rm.c](qnx/dev-quiver/pci_rm.c) | `pulse_attach()`, `SIGEV_PULSE_INIT()`, `timer_create(CLOCK_MONOTONIC, ...)` for periodic PCI-server health & recovery checks |
| **QNX CAN DDK (`-lcan`)** | [qnx/dev-can-quiver/quiver_can.c](qnx/dev-can-quiver/quiver_can.c), [Makefile](Makefile) | `can_drvr_funcs_t`, `can_drvr_register()`, split RX/TX `/dev/can0/rx0` & `/dev/can0/tx0`, CAN FD 64-byte frames, bus-off state |
| **QNX PCI Server (`-lpci`)** | [qnx/dev-quiver/pci_rm.c](qnx/dev-quiver/pci_rm.c), [qnx/dev-quiver/pci_service.c](qnx/dev-quiver/pci_service.c) | `pci_device_find()`, `pci_device_attach()`, `pci_device_read_ba()`, `pci_server-event_handler.so` |
| **QNX Security Policies (`-lsecpol`)** | [qnx/supervisor/quiver_supervisor.c](qnx/supervisor/quiver_supervisor.c), [qnx/security/qnx_quiver.policy](qnx/security/qnx_quiver.policy), [qnx/security/qemu_target_base.policy](qnx/security/qemu_target_base.policy) | `secpol_posix_spawnattr_settypeid()`, `secpol_resmgr_attach()`, `secpolcompile`, `secpolpush`, `secpolgenerate`, `secpolmonitor`, immutable policy `9db957c2` |
| **QNX System Logging (`-lslog2`) & Tracing** | [qnx/dev-can-quiver/quiver_can.c](qnx/dev-can-quiver/quiver_can.c), [qemu/build_hotplug_test_ifs.ps1](qemu/build_hotplug_test_ifs.ps1) | `slog2` logging, `slog2info`, `secpolmonitor` (`_tracelog`), `procnto-smp-instr` |

Live QNX 8.0 QEMU verification outputs (including `pidin -f a_n`, `secpol -Q`, `quiver-client-probe`, `quiver-can-probe`, `quiver-actuator`, `quiver-client-denied-probe`, and `quiver-pci-db-probe`) are recorded in [docs/implementation-status.md](docs/implementation-status.md).
