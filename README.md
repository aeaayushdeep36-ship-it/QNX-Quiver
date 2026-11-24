# QNX-Quiver

QNX-Quiver is a QNX SDP 8.0 virtual embedded hardware and driver project. Its intended target is QEMU x86_64, with a host plant simulator exchanging CAN, IMU, and actuator data through a shared-memory protocol.

## Current implementation status

The machine has QNX SDP 8.0 and the x86_64 CAN DDK. Windows QEMU 11.1.0 lacks ivshmem; Ubuntu WSL QEMU 10.2.1 provides it. The QNX guest has booted with `ivshmem-plain`, and live BAR2 visibility from a WSL host simulator has been verified. The QNX PCI resource manager exposes `/dev/quiver` and its restricted client `devctl` API. The libcan CAN-FD frontend publishes `/dev/can0/rx0` and `/dev/can0/tx0`; a 64-byte extended-ID frame has passed a guest-to-simulator-to-guest loopback check.

The implemented foundation includes the shared-memory protocol, host simulator, QNX cross-compiled layout check, WSL QEMU launcher, verified guest BAR2 access, and resident PCI resource manager. Use WSL for both QEMU and the host simulator when testing live shared memory; Windows and WSL mappings of the same DrvFS file did not reflect live writes across the boundary.

See docs/environment.md for the discovered environment and docs/protocol.md for the BAR2 layout and protocol constraints.

## Build and run

Build the protocol layout check from a QNX SDP 8.0 environment:

    qcc -Vgcc_ntox86_64 -std=c11 -Wall -Wextra -Werror -Iinclude -o build/protocol-check qnx/protocol_check.c
    build/protocol-check

Build the minimal PCI service from the project root in the same environment:

    make pci-service

Build the resident PCI resource manager:

    make pci-rm

Build the libcan CAN-FD endpoints, integration probe, and security-policy fragment check:

    make can-ddk can-probe policy-check

With the PCI resource manager running and the host simulator mapped to the same BAR2 backing file, run these two commands in the QNX guest:

    build/dev-can-quiver rx &
    build/dev-can-quiver tx &
    build/quiver-can-probe

The endpoint processes are separate because this QNX SDP 8.0 target image rejects a second libcan node attach from the same process. The frontend accepts CAN-FD payload lengths 0–8, 12, 16, 20, 24, 32, 48, and 64 bytes. The policy fragment is not a complete boot policy; merge it with the target baseline and add the PCI-server channel permission for that image.

Run the host simulator on Windows with Python 3.10 or newer:

    python host/sim_engine/quiver_sim.py --shm "%TEMP%\qnx_quiver_shm.bin"

The simulator creates and initializes a 4 MiB shared file when it does not exist. While it is running, enter fault busoff, fault crc, fault imu_stuck, latency 5000, clear, or status to update or inspect the shared fault bank. Use quit to stop the simulator. Use --self-test to check protocol initialization and ring operations without launching the physics loop. Add --duration 2 to run a short, measured smoke session.

For the WSL QEMU guest, run the simulator inside WSL against the same backing file:

    bash qemu/run_quiver_sim_wsl.sh

This keeps both mappings in the WSL environment. A bounded run can be passed through with `--duration 2`. The simulator exercises the host-side protocol; it is not a QNX driver and does not implement PCI attachment or device nodes.

## Project layout

- include/quiver_protocol.h: BAR2 data structures, offsets, atomics, and compile-time layout assertions.
- host/sim_engine/: shared-memory plant simulator and fault-injection console.
- qnx/: QNX target sources and tools; includes protocol checker and minimal PCI service.
- qemu/: target launch and capability checks.
- tests/: host protocol checks.

## Naming

The project name is QNX-Quiver. Protocol magic QVB1, PCI vendor/device IDs, and established device paths are wire or platform identifiers from the handoff and are retained as technical identifiers.
