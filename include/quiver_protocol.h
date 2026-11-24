#ifndef QNX_QUIVER_PROTOCOL_H
#define QNX_QUIVER_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#if defined(__GNUC__) || defined(__clang__)
#define QV_ALIGN64 __attribute__((aligned(64)))
#else
#define QV_ALIGN64
#endif

#define QV_PROTOCOL_MAGIC UINT32_C(0x51564231) /* QVB1 */
#define QV_PROTOCOL_VERSION UINT16_C(1)
#define QV_BAR2_BYTES UINT32_C(0x400000)
#define QV_SUPERBLOCK_OFFSET UINT32_C(0x000000)
#define QV_FAULT_BANK_OFFSET UINT32_C(0x001000)
#define QV_RING0_OFFSET UINT32_C(0x002000)
#define QV_RING1_OFFSET UINT32_C(0x082000)
#define QV_RING2_OFFSET UINT32_C(0x102000)
#define QV_RING3_OFFSET UINT32_C(0x182000)
#define QV_RING_BYTES UINT32_C(0x080000)
#define QV_RING_COUNT 4u
#define QV_RING_SLOT_BYTES 128u
#define QV_RING_SLOTS 4095u
#define QV_RING_CAPACITY (QV_RING_SLOTS - 1u)

#define QV_CAP_IVSHMEM_PLAIN UINT32_C(0x00000001)
#define QV_CAP_IVSHMEM_DOORBELL UINT32_C(0x00000002)

#define QV_FAULT_CAN_BUSOFF UINT32_C(0x00000001)
#define QV_FAULT_CAN_CRC_CORRUPT UINT32_C(0x00000002)
#define QV_FAULT_IMU_STUCK UINT32_C(0x00000004)
#define QV_FAULT_LATENCY_SPIKE UINT32_C(0x00000008)

#define QV_SLOT_TYPE_CAN UINT16_C(1)
#define QV_SLOT_TYPE_IMU UINT16_C(2)
#define QV_SLOT_TYPE_ACTUATOR UINT16_C(3)
#define QV_SLOT_FLAG_CRC_CORRUPT UINT16_C(1)

/* CAN ring payload: id:u32le, length:u8, flags:u8, reserved:u16le, data[64]. */
#define QV_CAN_WIRE_HEADER_BYTES 8u
#define QV_CAN_WIRE_DATA_BYTES 64u
#define QV_CAN_WIRE_BYTES (QV_CAN_WIRE_HEADER_BYTES + QV_CAN_WIRE_DATA_BYTES)
#define QV_CAN_WIRE_FLAG_CRC_CORRUPT UINT8_C(0x01)
#define QV_CAN_WIRE_FLAG_REMOTE UINT8_C(0x02)
#define QV_CAN_WIRE_FLAG_EXTENDED UINT8_C(0x04)
#define QV_CAN_WIRE_FLAG_FD UINT8_C(0x08)

typedef struct QV_ALIGN64 {
    uint32_t magic;
    uint16_t protocol_version;
    uint16_t superblock_bytes;
    uint32_t shared_bytes;
    uint32_t capabilities;
    _Atomic uint64_t host_heartbeat_ns;
    _Atomic uint64_t guest_heartbeat_ns;
    _Atomic uint32_t generation;
    _Atomic uint32_t host_status;
    _Atomic uint32_t guest_status;
    uint8_t reserved[20];
} qv_superblock_t;

typedef struct QV_ALIGN64 {
    _Atomic uint32_t fault_flags;
    _Atomic uint32_t latency_spike_us;
    _Atomic uint64_t host_can_tx;
    _Atomic uint64_t host_can_rx;
    _Atomic uint64_t qnx_can_tx;
    _Atomic uint64_t qnx_can_rx;
    _Atomic uint64_t dropped_frames;
    _Atomic uint64_t recovery_count;
    uint8_t reserved[8];
} qv_fault_bank_t;

typedef struct QV_ALIGN64 {
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    _Atomic uint32_t dropped;
    uint32_t slot_count;
    uint32_t slot_bytes;
    uint8_t reserved[44];
} qv_ring_control_t;

typedef struct QV_ALIGN64 {
    uint32_t sequence;
    uint16_t type;
    uint16_t flags;
    uint64_t timestamp_ns;
    uint8_t payload[112];
} qv_ring_slot_t;

typedef struct QV_ALIGN64 {
    qv_ring_control_t control;
    qv_ring_slot_t slots[QV_RING_SLOTS];
    uint8_t reserved[64];
} qv_ring_region_t;

typedef struct QV_ALIGN64 {
    qv_superblock_t superblock;
    uint8_t superblock_reserved[4096u - sizeof(qv_superblock_t)];
    qv_fault_bank_t faults;
    uint8_t fault_reserved[4096u - sizeof(qv_fault_bank_t)];
    qv_ring_region_t can_rx;
    qv_ring_region_t can_tx;
    qv_ring_region_t imu_rx;
    qv_ring_region_t actuator_tx;
    uint8_t reserved[QV_BAR2_BYTES - QV_RING3_OFFSET - QV_RING_BYTES];
} qv_bar2_t;

_Static_assert(sizeof(qv_superblock_t) == 64u, "superblock must occupy one cache line");
_Static_assert(sizeof(qv_fault_bank_t) == 64u, "fault bank header must occupy one cache line");
_Static_assert(sizeof(qv_ring_control_t) == 64u, "ring control must occupy one cache line");
_Static_assert(sizeof(qv_ring_slot_t) == QV_RING_SLOT_BYTES, "ring slot size mismatch");
_Static_assert(sizeof(qv_ring_region_t) == QV_RING_BYTES, "ring region size mismatch");
_Static_assert(offsetof(qv_bar2_t, faults) == QV_FAULT_BANK_OFFSET, "fault bank offset mismatch");
_Static_assert(offsetof(qv_bar2_t, can_rx) == QV_RING0_OFFSET, "CAN RX offset mismatch");
_Static_assert(offsetof(qv_bar2_t, can_tx) == QV_RING1_OFFSET, "CAN TX offset mismatch");
_Static_assert(offsetof(qv_bar2_t, imu_rx) == QV_RING2_OFFSET, "IMU offset mismatch");
_Static_assert(offsetof(qv_bar2_t, actuator_tx) == QV_RING3_OFFSET, "actuator offset mismatch");
_Static_assert(sizeof(qv_bar2_t) == QV_BAR2_BYTES, "BAR2 protocol must occupy 4 MiB");

#endif
