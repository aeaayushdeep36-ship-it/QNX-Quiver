#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "quiver_client.h"

int main(void)
{
    qv_client_t client = {.fd = -1};
    qv_client_info_t info;
    qv_client_faults_t faults;

    int status = qv_client_open(&client, NULL, O_RDONLY);
    if (status != 0) {
        fprintf(stderr, "QNX-Quiver client: open: %s (%d)\n",
                strerror(status), status);
        return 1;
    }
    status = qv_client_get_info(&client, &info);
    if (status != 0) {
        fprintf(stderr, "QNX-Quiver client: get info: %s (%d)\n",
                strerror(status), status);
        qv_client_close(&client);
        return 1;
    }
    status = qv_client_get_faults(&client, &faults);
    if (status != 0) {
        fprintf(stderr, "QNX-Quiver client: get faults: %s (%d)\n",
                strerror(status), status);
        qv_client_close(&client);
        return 1;
    }

    printf("QNX-Quiver client API v%" PRIu32 ", QVB1 v%u, BAR2=%" PRIu32
           " bytes, generation=%" PRIu32 "\n",
           info.api_version, info.protocol_version, info.bar_bytes,
           info.generation);
    printf("rings=%" PRIu32 " slots=%" PRIu32 " usable=%" PRIu32
           " slot_bytes=%" PRIu32 "\n",
           info.ring_count, info.ring_slots, info.ring_capacity,
           info.ring_slot_bytes);
    printf("fault_flags=0x%08" PRIx32 " latency_us=%" PRIu32
           " host_can_tx=%" PRIu64 " host_can_rx=%" PRIu64
           " qnx_can_tx=%" PRIu64 " qnx_can_rx=%" PRIu64
           " dropped=%" PRIu64 " recoveries=%" PRIu64 "\n",
           faults.fault_flags, faults.latency_spike_us,
           faults.host_can_tx, faults.host_can_rx, faults.qnx_can_tx,
           faults.qnx_can_rx, faults.dropped_frames, faults.recovery_count);
    qv_client_close(&client);
    return 0;
}
