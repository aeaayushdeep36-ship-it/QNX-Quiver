#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quiver_services.h"

static volatile sig_atomic_t stopping;
static void stop_handler(int sig) { (void)sig; stopping = 1; }

static void sleep_ms(long ms)
{
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    while (!stopping && nanosleep(&delay, &delay) == -1 && errno == EINTR) { }
}

static float clamp(float value)
{
    if (value > QV_ACTUATOR_VALUE_LIMIT) return QV_ACTUATOR_VALUE_LIMIT;
    if (value < -QV_ACTUATOR_VALUE_LIMIT) return -QV_ACTUATOR_VALUE_LIMIT;
    return value;
}

int main(void)
{
    (void)signal(SIGINT, stop_handler);
    (void)signal(SIGTERM, stop_handler);
    qv_client_t client = {.fd = -1};
    int status = qv_client_open(&client, QV_CLIENT_DEVICE_PATH, O_RDWR);
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver controller: open /dev/quiver: %s\n", strerror(status));
        return EXIT_FAILURE;
    }
    uint64_t last_sample = qv_service_monotonic_ns();
    uint64_t last_source_timestamp = 0;
    uint64_t command_id = qv_service_monotonic_ns();
    int stale = 0;
    puts("QNX-Quiver controller demo online (safe zero on stale IMU).");
    while (!stopping) {
        qv_ring_slot_t sample_slot;
        status = qv_client_ring_pop(&client, QV_RING_IMU_RX, &sample_slot);
        if (status == EOK) {
            qv_imu_sample_t sample;
            status = qv_imu_decode(&sample_slot, &sample);
            if (status == EOK) {
                if (sample_slot.timestamp_ns > last_source_timestamp) {
                    last_source_timestamp = sample_slot.timestamp_ns;
                    last_sample = qv_service_monotonic_ns();
                    stale = 0;
                    qv_ring_slot_t command;
                    /* Demonstration P controller: damp simulated X acceleration. */
                    float output = clamp(-0.25f * sample.accel[0]);
                    ++command_id;
                    if (qv_actuator_encode(&command, output, 100u,
                                           command_id) == EOK) {
                        status = qv_client_ring_push(&client,
                            QV_RING_ACTUATOR_TX, &command);
                        if (status != EOK) {
                            fprintf(stderr, "QNX-Quiver controller: actuator push: %s\n",
                                    strerror(status));
                            (void)qv_client_counter_add(&client,
                                QV_COUNTER_QNX_ACTUATOR_DROPPED, 1);
                        }
                    }
                }
            } else {
                fprintf(stderr, "QNX-Quiver controller: malformed IMU sample\n");
                (void)qv_client_counter_add(&client,
                    QV_COUNTER_QNX_IMU_DROPPED, 1);
            }
        } else if (status != EAGAIN && status != EWOULDBLOCK) {
            fprintf(stderr, "QNX-Quiver controller: IMU read: %s\n", strerror(status));
        }
        uint64_t now = qv_service_monotonic_ns();
        if (now - last_sample > UINT64_C(100000000) && !stale) {
            qv_ring_slot_t safe_command;
            ++command_id;
            if (qv_actuator_encode(&safe_command, 0.0f, 100u, command_id) == EOK &&
                qv_client_ring_push(&client, QV_RING_ACTUATOR_TX,
                                    &safe_command) != EOK) {
                (void)qv_client_counter_add(&client,
                    QV_COUNTER_QNX_ACTUATOR_DROPPED, 1);
            }
            fprintf(stderr, "QNX-Quiver controller: IMU stale; sent safe zero command\n");
            stale = 1;
        }
        sleep_ms(1);
    }
    qv_client_close(&client);
    return EXIT_SUCCESS;
}
