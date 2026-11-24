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
    size_t path_length = strnlen(path, QV_CLIENT_PATH_CAPACITY);
    if (path_length == 0 || path_length >= QV_CLIENT_PATH_CAPACITY) {
        return ENAMETOOLONG;
    }
    memcpy(client->path, path, path_length + 1);
    client->open_flags = open_flags;
    client->fd = open(path, open_flags);
    if (client->fd == -1) {
        int status = errno;
        client->path[0] = '\0';
        return status;
    }
    qv_client_info_t info;
    int status = qv_client_get_info(client, &info);
    if (status != EOK) {
        qv_client_close(client);
        return status;
    }
    return EOK;
}

int qv_client_reopen(qv_client_t *client)
{
    if (client == NULL || client->path[0] == '\0') {
        return EINVAL;
    }
    if (client->fd != -1) {
        (void)close(client->fd);
        client->fd = -1;
    }
    client->fd = open(client->path, client->open_flags);
    return client->fd == -1 ? errno : EOK;
}

static int retry_safe_read(qv_client_t *client, int command, void *data,
                           size_t data_size)
{
    int status = devctl(client->fd, command, data, (int)data_size, NULL);
    if (status != ENODEV && status != EBADF && status != EIO) {
        return status;
    }
    status = qv_client_reopen(client);
    if (status != EOK) return status;
    qv_client_info_t info;
    memset(&info, 0, sizeof(info));
    status = devctl(client->fd, QV_DCMD_GET_INFO, &info, sizeof(info), NULL);
    if (status != EOK) return status;
    if (info.api_version != QV_CLIENT_API_VERSION ||
        info.protocol_magic != QV_PROTOCOL_MAGIC ||
        info.protocol_version != QV_PROTOCOL_VERSION) {
        return EPROTO;
    }
    return devctl(client->fd, command, data, (int)data_size, NULL);
}

static int ensure_client_connected(qv_client_t *client)
{
    if (client == NULL || client->path[0] == '\0') return EINVAL;
    if (client->fd != -1) return EOK;
    qv_client_info_t info;
    return qv_client_get_info(client, &info);
}

static void remember_disconnect(qv_client_t *client, int status)
{
    if (client != NULL && (status == ENODEV || status == EBADF || status == EIO)) {
        if (client->fd != -1) (void)close(client->fd);
        client->fd = -1;
    }
}

void qv_client_close(qv_client_t *client)
{
    if (client != NULL && client->fd != -1) {
        (void)close(client->fd);
        client->fd = -1;
    }
    if (client != NULL) {
        client->path[0] = '\0';
        client->open_flags = 0;
    }
}

int qv_client_get_info(qv_client_t *client, qv_client_info_t *info)
{
    if (client == NULL || client->path[0] == '\0' || info == NULL) {
        return EINVAL;
    }
    memset(info, 0, sizeof(*info));
    int status = retry_safe_read(client, QV_DCMD_GET_INFO, info, sizeof(*info));
    if (status != EOK) return status;
    return info->api_version == QV_CLIENT_API_VERSION &&
                   info->protocol_magic == QV_PROTOCOL_MAGIC &&
                   info->protocol_version == QV_PROTOCOL_VERSION
               ? EOK
               : EPROTO;
}

int qv_client_ring_pop(qv_client_t *client, qv_ring_id_t ring,
                       qv_ring_slot_t *slot)
{
    if (client == NULL || client->path[0] == '\0' || slot == NULL ||
        (ring != QV_RING_CAN_RX && ring != QV_RING_IMU_RX)) {
        return EINVAL;
    }
    int status = ensure_client_connected(client);
    if (status != EOK) return status;
    qv_client_ring_io_t request;
    memset(&request, 0, sizeof(request));
    request.ring_id = (uint32_t)ring;
    status = devctl(client->fd, QV_DCMD_RING_POP, &request,
                    sizeof(request), NULL);
    remember_disconnect(client, status);
    if (status == EOK) {
        *slot = request.slot;
    }
    return status;
}

int qv_client_ring_push(qv_client_t *client, qv_ring_id_t ring,
                        const qv_ring_slot_t *slot)
{
    if (client == NULL || client->path[0] == '\0' || slot == NULL ||
        (ring != QV_RING_CAN_TX && ring != QV_RING_ACTUATOR_TX)) {
        return EINVAL;
    }
    int status = ensure_client_connected(client);
    if (status != EOK) return status;
    qv_client_ring_io_t request;
    memset(&request, 0, sizeof(request));
    request.ring_id = (uint32_t)ring;
    request.slot = *slot;
    status = devctl(client->fd, QV_DCMD_RING_PUSH, &request,
                    sizeof(request), NULL);
    remember_disconnect(client, status);
    return status;
}

int qv_client_get_faults(qv_client_t *client, qv_client_faults_t *faults)
{
    if (client == NULL || client->path[0] == '\0' || faults == NULL) {
        return EINVAL;
    }
    memset(faults, 0, sizeof(*faults));
    return retry_safe_read(client, QV_DCMD_GET_FAULTS, faults, sizeof(*faults));
}

int qv_client_counter_add(qv_client_t *client, qv_client_counter_t counter,
                          uint64_t amount)
{
    if (client == NULL || client->path[0] == '\0' || amount == 0) {
        return EINVAL;
    }
    int status = ensure_client_connected(client);
    if (status != EOK) return status;
    qv_client_counter_add_t request = {
        .counter = (uint32_t)counter,
        .reserved = 0,
        .amount = amount
    };
    status = devctl(client->fd, QV_DCMD_COUNTER_ADD, &request,
                    sizeof(request), NULL);
    remember_disconnect(client, status);
    return status;
}
