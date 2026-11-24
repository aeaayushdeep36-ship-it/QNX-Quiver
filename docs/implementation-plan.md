# QNX-Quiver implementation plan

## Objective

Build a QNX SDP 8.0 x86_64 virtual embedded platform that connects a deterministic host plant to QNX services through an ivshmem BAR2 protocol. Keep the host simulator, QNX components, and integration checks on the shared definitions in `include/quiver_protocol.h`.

## Constraints and verified baseline

- Use the existing QNX SDP 8.0 installation and guest image; guest launches must use snapshot mode.
- Windows QEMU 11.1.0 does not expose `ivshmem-plain`. The verified path is Ubuntu WSL QEMU 10.2.1.
- For live data exchange, launch QEMU and the Python simulator in WSL against the same `build/qnx_quiver_shm.bin`. Windows and WSL mappings of the same DrvFS file did not show live updates across the boundary.
- The guest sees the `1af4:1110` ivshmem device with BAR0 size 256 bytes and BAR2 size 4 MiB. QNX `pci-tool` read the QVB1 header and observed the simulator advancing the IMU ring head.
- A standalone QNX `libpci` probe faulted in this guest. Do not use that prototype as a driver base without first resolving the fault; follow the SDP 8.0 PCI resource-manager APIs and validate in the guest.

## Phases and acceptance gates

### 0. Environment and safe launch — complete

Record toolchain and guest paths, gate unsupported QEMU builds, use snapshot disk mode, and keep temporary credentials out of tracked project content.

**Acceptance:** launcher rejects QEMU without `ivshmem-plain`; supported WSL launcher starts the existing guest without modifying its base disk.

### 1. Protocol and host plant — complete

Define an aligned BAR2 layout with versioning, explicit ring ownership, fixed slot sizing, and fault controls. Provide a host plant with bounded-rate execution, ring operations, and fault injection. Compile a QNX layout checker.

**Acceptance:** host self-test covers initialization, full/empty behavior, wraparound, and fault controls; QNX cross-compile passes with warnings as errors; a bounded host run approaches 1 kHz.

### 2. Virtual PCI discovery and live mapping — verified by tools

Start QEMU with file-backed `ivshmem-plain`; inspect the guest device and BAR sizes. Run the WSL plant on the exact same backing file and verify live values through QNX BAR2 reads.

**Acceptance:** guest enumerates `1af4:1110`, BAR2 header matches protocol v1, and a changing ring index is observable in the guest. This gate currently passes through `pci-tool`; no project driver exists yet.

### 3. QNX PCI service and client interface — resident endpoint, PCI-server recovery, and initial devctl API implemented; hot-remove remains open

The one-shot bring-up service follows the SDP 8.0 PCI client APIs. The resident service acquires I/O privilege, discovers/claims the device, maps BAR2, validates protocol magic/version/size, and exposes `/dev/quiver`. Clients use the versioned `qv_client_*` devctl API for one-slot ring operations, info, fault snapshots, and QNX-owned telemetry counters; they cannot map BAR2 or modify host-owned fault controls. A one-second health check withdraws the node on a PCI-server loss or invalid BAR2, invalidates stale resources, retries attach/map, and republishes only after QVB1 validates. SIGINT/SIGTERM/SIGHUP cause node detach, unmap, and PCI detach. Startup without the PCI server/device fails with an explicit diagnostic.

**Acceptance:** the guest resident process remains running; clients can open/read `/dev/quiver`; status and info report QVB1 v1; fault snapshots are returned; SIGTERM removes the node and releases the device; restarting `pci-server` while the manager is attached withdraws and then recreates the node. The service/client API compile; info/fault reads, a host-to-QNX CAN RX pop, a QNX-to-host CAN TX push, and a QNX counter update passed in the live WSL guest. Full simulator-to-CAN-DDK integration is still open. QEMU's i440FX hot-unplug did not remove the ivshmem function from `query-pci`, so physical hot-remove/reinsert remains unverified. A production security policy also remains open.

### 4. CAN and peripheral services — in progress

The CAN-FD frontend uses the installed DDK and translates 64-byte raw frames, extended IDs, and CRC markers through the protocol rings. RX polling runs on a worker thread and blocking readers wait on a bounded queue; live guest tests verified wake-on-traffic, full 64-byte simulator echo, and clean SIGTERM cancellation/removal. RX and TX are separate libcan processes because the current guest refuses a second path attach in one process. A least-privilege policy fragment compiles, but runtime enforcement requires merging it with the product image and adding its PCI-server channel type. Bus-off/recovery reporting remains open. Add IMU and actuator endpoints with bounded queues and explicit stale-data behavior.

**Acceptance:** CAN-FD RX/TX through the guest, rings, and host simulator with a 64-byte extended-ID round trip and counters checked; blocking RX wake, SIGTERM cancellation, and node cleanup verified in the QNX guest. Production policy enforcement remains an image-integration acceptance item. Sensor/actuator records and injected bus-off, CRC, stuck-IMU, and latency faults must also produce observable, recoverable outcomes.

### 5. Supervisor, trace, and controller example — pending

Add a supervisor that starts services in dependency order, monitors health, restarts recoverable failures, and records a concise fault state. Add trace capture and a PID controller example using the service API rather than direct BAR access.

**Acceptance:** documented startup/shutdown sequence works repeatedly; trace captures initialization, traffic, fault injection, and recovery; controller example remains stable under nominal input and fails safe on stale or invalid data.

### 6. Codelab and handoff package — pending

Write a reproducible codelab with environment setup, launch commands, inspection commands, fault exercises, expected outputs, limitations, and cleanup. Package source/build instructions and test evidence without guest credentials or generated key material.

**Acceptance:** a new developer can follow the guide from a clean supported environment and reproduce the protocol check, guest enumeration, live BAR2 update, and later service-level tests.

## Immediate next implementation task

Integrate the policy fragment with a complete target image and verify PCI-server permissions under enforcement. Implement IMU and actuator services on the same API. Add a hotplug-capable QEMU topology or hardware test to validate actual device-removal events.

## Current status

Phases 0 and 1 are implemented. Phase 2 passes using QNX `pci-tool` and the WSL plant. Phase 3 has a guest-verified resident read-only status node, clean shutdown, and PCI-server disconnect/reconnect recovery. Phase 4 CAN-FD loopback, blocking-reader wake, and SIGTERM cleanup pass in the live guest. Actual PCI hot-remove/reinsert and runtime enforcement of the production privilege policy remain open. The rest of phases 4–6 remain open.
