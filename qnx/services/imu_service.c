#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quiver_services.h"

static void sleep_ms(long milliseconds)
{
    struct timespec delay = {milliseconds / 1000,
                             (milliseconds % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) { }
}

int main(int argc, char **argv)
{
    unsigned stale_ms = 100u;
    unsigned duration_s = 0u;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--stale-ms") == 0 && i + 1 < argc) {
            stale_ms = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_s = (unsigned)strtoul(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "usage: %s [--stale-ms 1..60000] [--duration seconds]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (stale_ms == 0u || stale_ms > 60000u) {
        fprintf(stderr, "QNX-Quiver IMU: stale timeout must be 1..60000 ms\n");
        return EXIT_FAILURE;
    }
    qv_client_t client = {.fd = -1};
    int status = qv_client_open(&client, QV_CLIENT_DEVICE_PATH, O_RDONLY);
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver IMU: open /dev/quiver: %s\n", strerror(status));
        return EXIT_FAILURE;
    }
    uint64_t started = qv_service_monotonic_ns();
    uint64_t last_sample = 0;
    uint64_t last_source_timestamp = 0;
    int stale_reported = 0;
    puts("QNX-Quiver IMU consumer online; timestamps are source-clock values.");
    while (duration_s == 0u ||
           qv_service_monotonic_ns() - started < (uint64_t)duration_s * 1000000000u) {
        qv_ring_slot_t slot;
        status = qv_client_ring_pop(&client, QV_RING_IMU_RX, &slot);
        if (status == EOK) {
            qv_imu_sample_t sample;
            status = qv_imu_decode(&slot, &sample);
            if (status != EOK) {
                fprintf(stderr, "QNX-Quiver IMU: rejected malformed sample\n");
                (void)qv_client_counter_add(&client,
                                             QV_COUNTER_QNX_IMU_DROPPED, 1);
                sleep_ms(1);
                continue;
            }
            uint64_t receive_time = qv_service_monotonic_ns();
            if (slot.timestamp_ns > last_source_timestamp) {
                last_source_timestamp = slot.timestamp_ns;
                last_sample = receive_time;
                stale_reported = 0;
            }
            printf("imu seq=%u source_ns=%llu accel=%.5f,%.5f,%.5f gyro=%.5f,%.5f,%.5f\n",
                   slot.sequence, (unsigned long long)slot.timestamp_ns,
                   sample.accel[0], sample.accel[1], sample.accel[2],
                   sample.gyro[0], sample.gyro[1], sample.gyro[2]);
        } else if (status != EAGAIN && status != EWOULDBLOCK) {
            fprintf(stderr, "QNX-Quiver IMU: ring read: %s\n", strerror(status));
            if (status == EPROTO) {
                (void)qv_client_counter_add(&client,
                                             QV_COUNTER_QNX_IMU_DROPPED, 1);
            }
        }
        uint64_t now = qv_service_monotonic_ns();
        uint64_t freshness_reference = last_sample != 0u ? last_sample : started;
        if (now - freshness_reference > (uint64_t)stale_ms * 1000000u &&
            !stale_reported) {
            fprintf(stderr, "QNX-Quiver IMU: STALE (no fresh sensor sample for %u ms)\n",
                    stale_ms);
            stale_reported = 1;
        }
        sleep_ms(1);
    }
    qv_client_close(&client);
    return EXIT_SUCCESS;
}
