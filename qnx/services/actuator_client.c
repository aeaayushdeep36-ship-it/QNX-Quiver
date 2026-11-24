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
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s value[-1..1] [ttl-ms 1..1000]\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *end = NULL;
    float value = strtof(argv[1], &end);
    if (end == argv[1] || *end != '\0') {
        fprintf(stderr, "QNX-Quiver actuator: value must be a number\n");
        return EXIT_FAILURE;
    }
    unsigned long ttl_value = argc == 3 ? strtoul(argv[2], &end, 10) : 100ul;
    if (argc == 3 && (end == argv[2] || *end != '\0' || ttl_value > UINT32_MAX)) {
        fprintf(stderr, "QNX-Quiver actuator: TTL must be an integer\n");
        return EXIT_FAILURE;
    }
    uint64_t command_id = qv_service_monotonic_ns();
    if (command_id == 0u) command_id = 1u;
    qv_ring_slot_t slot;
    int status = qv_actuator_encode(&slot, value, (uint32_t)ttl_value,
                                    command_id);
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver actuator: value/TTL outside safe bounds\n");
        return EXIT_FAILURE;
    }
    qv_client_t client = {.fd = -1};
    status = qv_client_open(&client, QV_CLIENT_DEVICE_PATH, O_RDWR);
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver actuator: open /dev/quiver: %s\n", strerror(status));
        return EXIT_FAILURE;
    }
    qv_client_faults_t before, after;
    status = qv_client_get_faults(&client, &before);
    if (status == EOK) status = qv_client_ring_push(&client, QV_RING_ACTUATOR_TX, &slot);
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver actuator: command publish: %s\n", strerror(status));
        if (status == EAGAIN || status == EPROTO) {
            (void)qv_client_counter_add(&client,
                                       QV_COUNTER_QNX_ACTUATOR_DROPPED, 1);
        }
        qv_client_close(&client);
        return EXIT_FAILURE;
    }
    uint64_t stop_at = qv_service_monotonic_ns() + UINT64_C(1500000000);
    for (;;) {
        status = qv_client_get_faults(&client, &after);
        if (status != EOK || after.host_actuator_last_id == command_id ||
            after.host_actuator_last_rejected_id == command_id ||
            qv_service_monotonic_ns() >= stop_at) break;
        sleep_ms(5);
    }
    if (status == EOK && after.host_actuator_last_id == command_id) {
        printf("actuator command %llu acknowledged; TTL=%lu ms\n",
               (unsigned long long)command_id, ttl_value);
    } else if (status == EOK &&
               after.host_actuator_last_rejected_id == command_id) {
        fprintf(stderr, "QNX-Quiver actuator: simulator rejected command\n");
        status = EPROTO;
    } else if (status == EOK) {
        fprintf(stderr, "QNX-Quiver actuator: no simulator acknowledgement\n");
        status = ETIMEDOUT;
    } else {
        fprintf(stderr, "QNX-Quiver actuator: acknowledgement read: %s\n", strerror(status));
    }
    qv_client_close(&client);
    return status == EOK ? EXIT_SUCCESS : EXIT_FAILURE;
}
