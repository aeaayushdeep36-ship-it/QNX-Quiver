#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/can_dcmd.h>
#include <time.h>
#include <unistd.h>

#include "quiver_client.h"

static void pause_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    (void)nanosleep(&delay, NULL);
}

static int blocking_rx_probe(void)
{
    CAN_MSG message;
    int fd = open("/dev/can0/rx0", O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "QNX-Quiver CAN probe: open RX node: %s\n", strerror(errno));
        return 1;
    }
    int status = devctl(fd, CAN_DEVCTL_RX_FRAME_RAW_BLOCK,
                        &message, sizeof(message), NULL);
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver CAN probe: blocking RX failed: %s\n",
                strerror(status));
        close(fd);
        return 1;
    }
    printf("QNX-Quiver CAN blocking RX woke: id=0x%08x len=%u\n",
           message.mid, message.len);
    close(fd);
    return 0;
}

static int drain_rx_probe(void)
{
    CAN_MSG message;
    int fd = open("/dev/can0/rx0", O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "QNX-Quiver CAN probe: open RX node: %s\n", strerror(errno));
        return 1;
    }
    while (devctl(fd, CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK,
                  &message, sizeof(message), NULL) == EOK) {
    }
    printf("QNX-Quiver CAN RX queue drained\n");
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--blocking-rx") == 0) {
        return blocking_rx_probe();
    }
    if (argc == 2 && strcmp(argv[1], "--drain-rx") == 0) {
        return drain_rx_probe();
    }
    if (argc == 2 && strcmp(argv[1], "--status") == 0) {
        qv_can_status_t can_status;
        int fd = open("/dev/can0/rx0", O_RDONLY);
        if (fd == -1) {
            fprintf(stderr, "QNX-Quiver CAN probe: open RX node: %s\n",
                    strerror(errno));
            return 1;
        }
        int status = devctl(fd, QV_CAN_DCMD_GET_STATUS, &can_status,
                            sizeof(can_status), NULL);
        close(fd);
        if (status != EOK || can_status.api_version != QV_CLIENT_API_VERSION) {
            fprintf(stderr, "QNX-Quiver CAN probe: status query failed: %s\n",
                    strerror(status != EOK ? status : EPROTO));
            return 1;
        }
        printf("CAN status: %s fault_flags=0x%08x generation=%u\n",
               can_status.bus_off ? "bus-off" : "active",
               can_status.fault_flags, can_status.state_generation);
        return 0;
    }
    qv_client_t client = {.fd = -1};
    qv_client_faults_t before, after;
    CAN_MSG tx, rx;
    int tx_fd = -1, rx_fd = -1, status, received = 0;

    status = qv_client_open(&client, QV_CLIENT_DEVICE_PATH, O_RDONLY);
    if (status != EOK || qv_client_get_faults(&client, &before) != EOK) {
        fprintf(stderr, "QNX-Quiver CAN probe: cannot read service counters\n");
        qv_client_close(&client);
        return 1;
    }
    tx_fd = open("/dev/can0/tx0", O_WRONLY);
    rx_fd = open("/dev/can0/rx0", O_RDONLY | O_NONBLOCK);
    if (tx_fd == -1 || rx_fd == -1) {
        fprintf(stderr, "QNX-Quiver CAN probe: open CAN nodes: %s\n", strerror(errno));
        if (tx_fd != -1) close(tx_fd);
        if (rx_fd != -1) close(rx_fd);
        qv_client_close(&client);
        return 1;
    }

    memset(&tx, 0, sizeof(tx));
    tx.mid = UINT32_C(0x1abcde);
    tx.len = CAN_MSG_DATA_MAX_CANFD;
    tx.ext.is_extended_mid = 1;
    tx.ext.timestamp = (uint32_t)(time(NULL) * 1000u);
    for (size_t i = 0; i < sizeof(tx.dat); ++i) {
        tx.dat[i] = (uint8_t)(i ^ 0x5au);
    }
    status = devctl(tx_fd, CAN_DEVCTL_TX_FRAME_RAW, &tx, sizeof(tx), NULL);
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver CAN probe: TX failed: %s\n", strerror(status));
        close(tx_fd);
        close(rx_fd);
        qv_client_close(&client);
        return 1;
    }

    for (int attempt = 0; attempt < 300; ++attempt) {
        if (qv_client_get_faults(&client, &after) == EOK &&
            after.host_can_rx > before.host_can_rx &&
            after.qnx_can_tx > before.qnx_can_tx) {
            break;
        }
        pause_ms(10);
    }
    for (int attempt = 0; attempt < 300; ++attempt) {
        status = devctl(rx_fd, CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK, &rx, sizeof(rx), NULL);
        if (status == EOK) {
            if (rx.mid == tx.mid && rx.len == CAN_MSG_DATA_MAX_CANFD &&
                rx.ext.is_extended_mid && memcmp(rx.dat, tx.dat, sizeof(tx.dat)) == 0) {
                received = 1;
                break;
            }
        } else if (status != EAGAIN && status != EWOULDBLOCK) {
            fprintf(stderr, "QNX-Quiver CAN probe: RX failed: %s\n", strerror(status));
            close(tx_fd);
            close(rx_fd);
            qv_client_close(&client);
            return 1;
        }
        pause_ms(10);
    }
    (void)qv_client_get_faults(&client, &after);
    printf("QNX-Quiver CAN-FD probe: tx_id=0x%08x tx_len=%u qnx_can_tx=%llu host_can_rx=%llu",
           tx.mid, tx.len,
           (unsigned long long)after.qnx_can_tx,
           (unsigned long long)after.host_can_rx);
    if (received) {
        printf(" echo_id=0x%08x echo_len=%u data64=verified\n", rx.mid, rx.len);
    } else {
        printf(" host RX record not received\n");
    }
    close(tx_fd);
    close(rx_fd);
    qv_client_close(&client);
    return received && after.qnx_can_tx > before.qnx_can_tx &&
           after.host_can_rx > before.host_can_rx ? 0 : 1;
}
