#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "quiver_client.h"

int qv_client_open(qv_client_t *client, const char *path, int open_flags)
{
    if (client == NULL) {
        return EINVAL;
    }
    client->fd = -1;
    if (path == NULL) {
        path = QV_CLIENT_DEVICE_PATH;
    }
    client->fd = open(path, open_flags);
    return client->fd == -1 ? errno : EOK;
}

void qv_client_close(qv_client_t *client)
{
    if (client != NULL && client->fd != -1) {
        (void)close(client->fd);
        client->fd = -1;
    }
}

int qv_client_get_info(qv_client_t *client, qv_client_info_t *info)
{
    if (client == NULL || client->fd == -1 || info == NULL) {
        return EINVAL;
    }
    memset(info, 0, sizeof(*info));
    return devctl(client->fd, QV_DCMD_GET_INFO, info, sizeof(*info), NULL);
}

int qv_client_ring_pop(qv_client_t *client, qv_ring_id_t ring,
                       qv_ring_slot_t *slot)
{
    if (client == NULL || client->fd == -1 || slot == NULL ||
        (ring != QV_RING_CAN_RX && ring != QV_RING_IMU_RX)) {
        return EINVAL;
    }
    qv_client_ring_io_t request;
    memset(&request, 0, sizeof(request));
    request.ring_id = (uint32_t)ring;
    int status = devctl(client->fd, QV_DCMD_RING_POP, &request,
                        sizeof(request), NULL);
    if (status == EOK) {
        *slot = request.slot;
    }
    return status;
}

int qv_client_ring_push(qv_client_t *client, qv_ring_id_t ring,
                        const qv_ring_slot_t *slot)
{
    if (client == NULL || client->fd == -1 || slot == NULL ||
        (ring != QV_RING_CAN_TX && ring != QV_RING_ACTUATOR_TX)) {
        return EINVAL;
    }
    qv_client_ring_io_t request;
    memset(&request, 0, sizeof(request));
    request.ring_id = (uint32_t)ring;
    request.slot = *slot;
    return devctl(client->fd, QV_DCMD_RING_PUSH, &request,
                  sizeof(request), NULL);
}

int qv_client_get_faults(qv_client_t *client, qv_client_faults_t *faults)
{
    if (client == NULL || client->fd == -1 || faults == NULL) {
        return EINVAL;
    }
    memset(faults, 0, sizeof(*faults));
    return devctl(client->fd, QV_DCMD_GET_FAULTS, faults, sizeof(*faults), NULL);
}

int qv_client_counter_add(qv_client_t *client, qv_client_counter_t counter,
                          uint64_t amount)
{
    if (client == NULL || client->fd == -1 || amount == 0) {
        return EINVAL;
    }
    qv_client_counter_add_t request = {
        .counter = (uint32_t)counter,
        .reserved = 0,
        .amount = amount
    };
    return devctl(client->fd, QV_DCMD_COUNTER_ADD, &request,
                  sizeof(request), NULL);
}
