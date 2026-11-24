#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quiver_services.h"

static void sleep_ms(long ms)
{
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) { }
}

int main(int argc, char **argv)
{
    const char *path = "/tmp/quiver-trace.csv";
    unsigned duration_s = 0u;
    unsigned long max_records = 10000ul;
    int argi = 1;
    if (argi < argc && strncmp(argv[argi], "--", 2) != 0) path = argv[argi++];
    while (argi < argc) {
        const char *option = argv[argi++];
        if (argi >= argc) goto usage;
        char *end = NULL;
        errno = 0;
        unsigned long value = strtoul(argv[argi++], &end, 10);
        if (errno != 0 || end == argv[argi - 1] || *end != '\0') goto usage;
        if (strcmp(option, "--duration") == 0 && value <= 86400ul) {
            duration_s = (unsigned)value;
        } else if (strcmp(option, "--records") == 0 &&
                   value > 0ul && value <= 1000000ul) {
            max_records = value;
        } else {
            goto usage;
        }
    }
    FILE *output = fopen(path, "w");
    if (output == NULL) {
        fprintf(stderr, "QNX-Quiver trace: open %s: %s\n", path, strerror(errno));
        return EXIT_FAILURE;
    }
    qv_client_t client = {.fd = -1};
    int status = qv_client_open(&client, QV_CLIENT_DEVICE_PATH, O_RDONLY);
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver trace: open /dev/quiver: %s\n", strerror(status));
        fclose(output);
        return EXIT_FAILURE;
    }
    fputs("monotonic_ns,fault_flags,latency_us,host_can_tx,host_can_rx,qnx_can_tx,qnx_can_rx,host_dropped,qnx_can_dropped,host_actuator_accepted,host_actuator_rejected,host_imu_dropped,host_seq,qnx_seq,qnx_imu_dropped,qnx_actuator_dropped\n",
          output);
    uint64_t started = qv_service_monotonic_ns();
    unsigned long records = 0ul;
    int last_status = EOK;
    puts("QNX-Quiver trace capture online.");
    while (records < max_records &&
           (duration_s == 0u || qv_service_monotonic_ns() - started <
            (uint64_t)duration_s * 1000000000u)) {
        qv_client_faults_t f;
        status = qv_client_get_faults(&client, &f);
        if (status == EOK) {
            (void)fprintf(output,
                "%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%llu,%llu\n",
                (unsigned long long)qv_service_monotonic_ns(), f.fault_flags,
                f.latency_spike_us, (unsigned long long)f.host_can_tx,
                (unsigned long long)f.host_can_rx, (unsigned long long)f.qnx_can_tx,
                (unsigned long long)f.qnx_can_rx, (unsigned long long)f.dropped_frames,
                (unsigned long long)f.qnx_can_dropped,
                (unsigned long long)f.host_actuator_accepted,
                (unsigned long long)f.host_actuator_rejected,
                (unsigned long long)f.host_imu_dropped,
                f.host_generation, f.qnx_generation,
                (unsigned long long)f.qnx_imu_dropped,
                (unsigned long long)f.qnx_actuator_dropped);
            (void)fflush(output);
            ++records;
            last_status = EOK;
        } else if (status != EAGAIN && status != last_status) {
            fprintf(stderr, "QNX-Quiver trace: fault snapshot: %s\n", strerror(status));
            last_status = status;
        }
        sleep_ms(100);
    }
    qv_client_close(&client);
    fclose(output);
    printf("QNX-Quiver trace saved: %s (%lu records)\n", path, records);
    return EXIT_SUCCESS;

usage:
    fprintf(stderr, "usage: %s [output.csv] [--duration seconds 0..86400] "
                    "[--records 1..1000000] (default record limit 10000)\n",
            argv[0]);
    return EXIT_FAILURE;
}
