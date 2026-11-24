#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/can_dcmd.h>
#include <unistd.h>

#include "quiver_protocol.h"

int main(void)
{
    CAN_MSG message;
    const unsigned max_attempts = QV_RING_CAPACITY + 64u;
    unsigned accepted = 0;
    int backpressure = 0;
    int fd = open("/dev/can0/tx0", O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "QNX-Quiver CAN saturation: open TX node: %s\n",
                strerror(errno));
        return 1;
    }

    memset(&message, 0, sizeof(message));
    message.len = CAN_MSG_DATA_MAX_CANFD;
    message.ext.is_extended_mid = 1;
    for (unsigned i = 0; i < max_attempts; ++i) {
        message.mid = UINT32_C(0x100000) | (i & UINT32_C(0x7ffff));
        message.ext.timestamp = i;
        for (size_t j = 0; j < sizeof(message.dat); ++j)
            message.dat[j] = (uint8_t)(i + j);

        int status = devctl(fd, CAN_DEVCTL_TX_FRAME_RAW, &message,
                            sizeof(message), NULL);
        if (status == EOK) {
            ++accepted;
        } else if (status == EAGAIN || status == EWOULDBLOCK) {
            backpressure = 1;
            break;
        } else {
            fprintf(stderr,
                    "QNX-Quiver CAN saturation: unexpected TX result after %u frames: %s\n",
                    accepted, strerror(status));
            close(fd);
            return 1;
        }
    }
    close(fd);

    printf("QNX-Quiver CAN saturation: accepted=%u capacity=%u backpressure=%s\n",
           accepted, QV_RING_CAPACITY, backpressure ? "EAGAIN" : "missing");
    return backpressure && accepted > 0 && accepted <= QV_RING_CAPACITY
               ? 0 : 1;
}
