# Shared-memory protocol

BAR2 is a 4 MiB, little-endian shared region. Protocol v3 keeps the established ring offsets and sizes while extending fault telemetry. QNX uses C11 acquire/release operations. The Python simulator uses aligned scalar accesses on Windows x86-64 and `libatomic` on Linux; its local mutex coordinates threads in one simulator process. Run exactly one host simulator process per backing file. Only x86-64 has been validated; other CPU and device-memory ordering models are outside the current support boundary.

## Layout

| BAR2 offset | Size | Purpose |
| --- | ---: | --- |
| 0x0000 | 4 KiB | Superblock: QVB1 version, heartbeat, generation |
| 0x1000 | 4 KiB | Fault flags, latency, CAN/IMU/actuator counters, snapshot generations |
| 0x2000 | 512 KiB | Host to QNX CAN RX |
| 0x82000 | 512 KiB | QNX to host CAN TX |
| 0x102000 | 512 KiB | Host to QNX IMU |
| 0x182000 | 512 KiB | QNX to host actuator commands |
| 0x202000 | remainder | Reserved |

Each ring has a 64-byte control block, 4095 128-byte slots, and 64 bytes of reserved padding. One slot stays empty to distinguish full from empty, leaving 4094 usable slots.

CAN payloads contain `id:u32le`, `length:u8`, `flags:u8`, `reserved:u16le`, then 64 data bytes. The DDK and host simulator validate legal CAN-FD lengths, identifier range and format, and remote-frame support.

Actuator payloads contain `axis:u32le`, `value:f32le`, `ttl_ms:u32le`, `reserved:u32le`, and `command_id:u64le`. The virtual plant accepts axis zero, finite values in `[-1,1]`, command IDs other than zero, and TTLs from 1 to 1000 ms. The TTL begins when the simulator accepts the command. It exports the last accepted/rejected ID and current output so clients can confirm a command; the output returns to zero after expiry. This models a virtual fail-safe only and is not a certified hardware safe state.

## Ownership and client semantics

The host and guest must use the same protocol version and capacity. A writer fills a slot before publishing the head; a reader advances the tail after consumption. Each ring has one logical producer and consumer. The PCI resource manager serializes operations but does not reserve a ring exclusively per client: multiple clients deliberately work-share one tail and divide records in dispatch order. They do not each receive a copy. `quiver-imu` and `quiver-controller` therefore cannot be run together as independent readers unless a fan-out broker is added.

QNX programs use `/dev/quiver` via `qv_client_*` in `include/quiver_client.h`. They never map BAR2 directly.

| Operation | Allowed ring/fields | Ownership |
| --- | --- | --- |
| Pop | CAN RX (0), IMU RX (2) | QNX consumer advances tail |
| Push | CAN TX (1), actuator TX (3) | QNX producer advances head |
| Fault snapshot | Flags, latency, CAN/IMU/actuator telemetry | Read only |
| Counter update | QNX CAN TX/RX, QNX CAN drops, recovery | QNX-owned fields |

Host-to-QNX data cannot be overwritten by QNX clients, and QNX-to-host rings cannot be consumed by them. Fault flags and injected latency remain host-controlled. Separate host/QNX sequence counters protect snapshots; a reader accepts data only when both sequence values stay even and unchanged. Snapshot retries are bounded, so a crashed writer returns an error instead of hanging the resource manager or simulator. Empty/full rings return `EAGAIN`; invalid metadata and malformed slot types return `EPROTO`. The client ABI is independently versioned and compile-time size assertions guard its structures.

The host exports separate CAN drops, IMU ring drops, accepted/rejected actuator commands, the last command IDs, and current actuator output. QNX exports CAN, IMU, and actuator-client drop counters. The QNX CAN frontend exposes a status devctl with bus-off state and a state generation incremented on active/off transitions.

## Fault behavior and validation boundary

- `CAN_BUSOFF`: suppresses simulated CAN traffic; the DDK endpoint reports bus-off and recovery transitions.
- `CAN_CRC_CORRUPT`: marks generated CAN frames with the CRC fault flag.
- `IMU_STUCK`: holds the most recent simulated sample and its source timestamp; QNX detects the non-advancing timestamp using its local receive clock.
- `LATENCY_SPIKE`: applies the configured host-side delay.

CAN loopback is a virtual bus test. It proves payload transport through QEMU and the QNX DDK frontend; it does not validate physical arbitration, bit timing, controller error counters, or transceiver behavior.

For coherent live updates, run WSL QEMU and the simulator inside WSL against the same `qnx_quiver_shm_v3.bin`. The Windows QEMU build does not provide `ivshmem-plain`.
