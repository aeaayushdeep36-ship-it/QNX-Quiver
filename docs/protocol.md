# Shared-memory protocol

The BAR2 protocol uses a 4 MiB region with little-endian fields. Control and ring indices use aligned 32-bit atomics. The QNX side uses C11 acquire/release operations. The Windows Python simulator runs only on x86-64 and uses aligned 32-bit and 64-bit scalar accesses, relying on x86-64 atomic load/store behavior and TSO ordering. Linux uses libatomic where available. The QNX BAR2 consumer and producer still use C11 atomics as declared in the shared header.

## Layout

| BAR2 offset | Size | Purpose |
| --- | ---: | --- |
| 0x0000 | 4 KiB | Superblock (QVB1, version, heartbeat, generation) |
| 0x1000 | 4 KiB | Fault flags, injected latency, frame and recovery counters |
| 0x2000 | 512 KiB | Host to QNX CAN RX |
| 0x82000 | 512 KiB | QNX to host CAN TX |
| 0x102000 | 512 KiB | Host to QNX IMU |
| 0x182000 | 512 KiB | QNX to host actuator commands |
| 0x202000 | remainder | Reserved |

Each ring has a 64-byte control block, 4095 128-byte slots, and 64 bytes of reserved padding. One slot is left unused to distinguish full from empty, leaving 4094 usable entries. A CAN payload is 72 bytes: an 8-byte header (`id:u32le`, `length:u8`, `flags:u8`, `reserved:u16le`) followed by 64 data bytes. Wire flags identify CRC corruption, remote frames, extended IDs, and CAN FD. The DDK frontend requires the FD flag, preserves extended-ID/CRC metadata, and rejects remote frames and invalid CAN-FD data lengths.

The host and guest must use the same header and ring capacity. Ring writers publish their head index only after writing a slot; readers advance the tail after consuming it. Each ring has exactly one producer and one consumer.

## QNX client access

QNX services use `/dev/quiver` and the `qv_client_*` API in `include/quiver_client.h`; they do not map BAR2. The resource manager serializes requests through its dispatch loop and validates the ring layout and ring direction on every operation.

| Operation | Allowed ring/fields | Ownership |
| --- | --- | --- |
| Pop one slot | CAN RX (0), IMU RX (2) | QNX consumer advances tail |
| Push one slot | CAN TX (1), actuator TX (3) | QNX producer advances head |
| Read fault snapshot | Fault flags, latency, counters | Snapshot only |
| Add telemetry | `qnx_can_tx`, `qnx_can_rx`, `recovery_count` | QNX-owned counters only |

Host-to-QNX data cannot be overwritten by QNX clients; QNX-to-host rings cannot be consumed by QNX clients. The fault flags and latency value remain host-controlled. Fault snapshots load each field atomically, but multiple counters are not captured as one transaction. Empty/full operations return `EAGAIN`; invalid ring metadata or slot types return `EPROTO`. The client ABI is versioned separately from QVB1 and pins structure sizes with compile-time assertions.

## Fault flags

- CAN_BUSOFF: stop normal CAN traffic and report bus-off.
- CAN_CRC_CORRUPT: mark generated CAN frames with the injected CRC fault flag.
- IMU_STUCK: hold the most recent IMU sample.
- LATENCY_SPIKE: apply the configured host-side delay in microseconds.

The host simulator echoes validated CAN-FD frames from QNX TX ring 1 into host
RX ring 0. This loopback is a virtual-bus test path, not a physical CAN
controller or arbitration model.

The BAR2 layout is a project-local protocol. The WSL QEMU launcher has verified live access to this BAR through `ivshmem-plain`; the installed Windows QEMU build does not provide that device. For coherent live host/guest updates, run both WSL QEMU and the simulator inside WSL against the same backing file.
