#ifndef QNX_QUIVER_CLIENT_H
#define QNX_QUIVER_CLIENT_H

#include <devctl.h>
#include <stdint.h>

#include "quiver_protocol.h"

#define QV_CLIENT_DEVICE_PATH "/dev/quiver"
#define QV_CLIENT_API_VERSION UINT32_C(1)

typedef enum {
    QV_RING_CAN_RX = 0,
    QV_RING_CAN_TX = 1,
    QV_RING_IMU_RX = 2,
    QV_RING_ACTUATOR_TX = 3
} qv_ring_id_t;

typedef struct {
    uint32_t api_version;
    uint32_t protocol_magic;
    uint16_t protocol_version;
    uint16_t reserved0;
    uint32_t bar_bytes;
    uint32_t capabilities;
    uint32_t generation;
    uint32_t ring_count;
    uint32_t ring_slots;
    uint32_t ring_capacity;
    uint32_t ring_slot_bytes;
    uint32_t reserved[6];
} qv_client_info_t;

typedef struct {
    uint32_t ring_id;
    uint32_t reserved;
    qv_ring_slot_t slot;
} qv_client_ring_io_t;

typedef struct {
    uint32_t fault_flags;
    uint32_t latency_spike_us;
    uint64_t host_can_tx;
    uint64_t host_can_rx;
    uint64_t qnx_can_tx;
    uint64_t qnx_can_rx;
    uint64_t dropped_frames;
    uint64_t recovery_count;
} qv_client_faults_t;

typedef enum {
    QV_COUNTER_QNX_CAN_TX = 1,
    QV_COUNTER_QNX_CAN_RX = 2,
    QV_COUNTER_RECOVERY = 3
} qv_client_counter_t;

typedef struct {
    uint32_t counter;
    uint32_t reserved;
    uint64_t amount;
} qv_client_counter_add_t;

_Static_assert(sizeof(qv_client_info_t) == 64u,
               "client info ABI must remain 64 bytes");
_Static_assert(offsetof(qv_client_ring_io_t, slot) == 64u,
               "client ring payload alignment changed");
_Static_assert(sizeof(qv_client_ring_io_t) == 192u,
               "client ring request ABI changed");
_Static_assert(sizeof(qv_client_faults_t) == 56u,
               "client fault snapshot ABI changed");
_Static_assert(sizeof(qv_client_counter_add_t) == 16u,
               "client counter request ABI changed");

#define QV_DCMD_GET_INFO       __DIOF(_DCMD_MISC, 0xD0, qv_client_info_t)
#define QV_DCMD_RING_POP       __DIOTF(_DCMD_MISC, 0xD1, qv_client_ring_io_t)
#define QV_DCMD_RING_PUSH      __DIOT(_DCMD_MISC, 0xD2, qv_client_ring_io_t)
#define QV_DCMD_GET_FAULTS     __DIOF(_DCMD_MISC, 0xD3, qv_client_faults_t)
#define QV_DCMD_COUNTER_ADD    __DIOT(_DCMD_MISC, 0xD4, qv_client_counter_add_t)

typedef struct {
    int fd;
} qv_client_t;

int qv_client_open(qv_client_t *client, const char *path, int open_flags);
void qv_client_close(qv_client_t *client);
int qv_client_get_info(qv_client_t *client, qv_client_info_t *info);
int qv_client_ring_pop(qv_client_t *client, qv_ring_id_t ring,
                       qv_ring_slot_t *slot);
int qv_client_ring_push(qv_client_t *client, qv_ring_id_t ring,
                        const qv_ring_slot_t *slot);
int qv_client_get_faults(qv_client_t *client, qv_client_faults_t *faults);
int qv_client_counter_add(qv_client_t *client, qv_client_counter_t counter,
                          uint64_t amount);

#endif
