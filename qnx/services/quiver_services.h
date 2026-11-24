#ifndef QNX_QUIVER_SERVICES_H
#define QNX_QUIVER_SERVICES_H

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "quiver_client.h"

typedef struct {
    float accel[3];
    float gyro[3];
} qv_imu_sample_t;

_Static_assert(sizeof(qv_imu_sample_t) == 24u, "IMU wire sample size changed");

static inline uint64_t qv_service_monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static inline int qv_imu_decode(const qv_ring_slot_t *slot,
                               qv_imu_sample_t *sample)
{
    if (slot == NULL || sample == NULL || slot->type != QV_SLOT_TYPE_IMU) {
        return EPROTO;
    }
    memcpy(sample, slot->payload, sizeof(*sample));
    for (unsigned i = 0; i < 3u; ++i) {
        if (!isfinite(sample->accel[i]) || !isfinite(sample->gyro[i])) {
            return EPROTO;
        }
    }
    return EOK;
}

static inline int qv_actuator_encode(qv_ring_slot_t *slot, float value,
                                     uint32_t ttl_ms, uint64_t command_id)
{
    if (slot == NULL || !isfinite(value) ||
        value < -QV_ACTUATOR_VALUE_LIMIT || value > QV_ACTUATOR_VALUE_LIMIT ||
        ttl_ms == 0u || ttl_ms > QV_ACTUATOR_TTL_MAX_MS || command_id == 0u) {
        return EINVAL;
    }
    memset(slot, 0, sizeof(*slot));
    slot->type = QV_SLOT_TYPE_ACTUATOR;
    slot->timestamp_ns = qv_service_monotonic_ns();
    memcpy(slot->payload + 4, &value, sizeof(value));
    memcpy(slot->payload + 8, &ttl_ms, sizeof(ttl_ms));
    memcpy(slot->payload + 16, &command_id, sizeof(command_id));
    return EOK;
}

#endif
