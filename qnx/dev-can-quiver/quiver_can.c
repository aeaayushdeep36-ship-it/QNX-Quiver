#include <errno.h>
#include <fcntl.h>
#include <hw/libcan.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/can_dcmd.h>
#include <sys/iomsg.h>
#include <time.h>
#include <unistd.h>

#include "quiver_client.h"

#define QV_CAN_UNIT 0
#define QV_CAN_RX_UNIT 0
#define QV_CAN_TX_UNIT 0
#define QV_CAN_QUEUE_SIZE 64u
#define QV_CAN_POLL_NS 1000000L
#define QV_CAN_RX_QUEUE_SIZE 64u

typedef struct {
    qv_client_t client;
    CANDEV rx;
    CANDEV tx;
    int initialized;
    int rx_initialized;
    int tx_initialized;
    int rx_created;
    int tx_created;
    int rx_worker_started;
    volatile sig_atomic_t rx_stop;
    pthread_t rx_worker;
    pthread_t signal_watcher;
    uint64_t rx_count;
    uint64_t tx_count;
    uint64_t dropped_count;
} quiver_can_t;

static quiver_can_t g_can = {.client = {.fd = -1}};
static pthread_mutex_t g_rx_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rx_ready = PTHREAD_COND_INITIALIZER;
static CAN_MSG g_rx_queue[QV_CAN_RX_QUEUE_SIZE];
static unsigned g_rx_head;
static unsigned g_rx_tail;
static unsigned g_rx_count;
static struct sigaction g_initial_sigterm;
static struct sigaction g_libcan_sigterm;
static volatile sig_atomic_t g_signal_watch_stop;
static int g_signal_watch_started;
static int g_signal_hooked;

static int sleep_poll_interval(void)
{
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = QV_CAN_POLL_NS};
    return nanosleep(&interval, NULL);
}

static int can_fd_length_valid(uint8_t length)
{
    return length <= 8 || length == 12 || length == 16 || length == 20 ||
           length == 24 || length == 32 || length == 48 || length == 64;
}

static void slot_to_can_msg(const qv_ring_slot_t *slot, CAN_MSG *message)
{
    uint8_t length = slot->payload[4];
    memset(message, 0, sizeof(*message));
    message->mid = (uint32_t)slot->payload[0] |
                   ((uint32_t)slot->payload[1] << 8) |
                   ((uint32_t)slot->payload[2] << 16) |
                   ((uint32_t)slot->payload[3] << 24);
    message->len = length;
    message->ext.timestamp = (uint32_t)(slot->timestamp_ns / UINT64_C(1000000));
    message->ext.is_extended_mid =
        (slot->payload[5] & QV_CAN_WIRE_FLAG_EXTENDED) != 0;
    message->ext.is_remote_frame =
        (slot->payload[5] & QV_CAN_WIRE_FLAG_REMOTE) != 0;
    message->rsv[0] = (uint8_t)(slot->flags & QV_SLOT_FLAG_CRC_CORRUPT);
    if (length <= CAN_MSG_DATA_MAX_CANFD) {
        memcpy(message->dat, slot->payload + 8, length);
    }
}

static int can_msg_to_slot(const CAN_MSG *message, qv_ring_slot_t *slot)
{
    if (!can_fd_length_valid(message->len) ||
        message->mid > UINT32_C(0x1fffffff) || message->ext.is_remote_frame) {
        return EINVAL;
    }
    if (!message->ext.is_extended_mid && message->mid > UINT32_C(0x7ff)) {
        return EINVAL;
    }
    memset(slot, 0, sizeof(*slot));
    slot->type = QV_SLOT_TYPE_CAN;
    slot->flags = (uint16_t)(message->rsv[0] & QV_SLOT_FLAG_CRC_CORRUPT);
    slot->timestamp_ns = (uint64_t)message->ext.timestamp * UINT64_C(1000000);
    slot->payload[0] = (uint8_t)message->mid;
    slot->payload[1] = (uint8_t)(message->mid >> 8);
    slot->payload[2] = (uint8_t)(message->mid >> 16);
    slot->payload[3] = (uint8_t)(message->mid >> 24);
    slot->payload[4] = message->len;
    slot->payload[5] = QV_CAN_WIRE_FLAG_FD;
    if (message->ext.is_extended_mid) {
        slot->payload[5] |= QV_CAN_WIRE_FLAG_EXTENDED;
    }
    if (slot->flags & QV_SLOT_FLAG_CRC_CORRUPT) {
        slot->payload[5] |= QV_CAN_WIRE_FLAG_CRC_CORRUPT;
    }
    memcpy(slot->payload + 8, message->dat, message->len);
    return EOK;
}

/* Host simulator wire record: id:u32le, len:u8, flags:u8, reserved:u16,
 * followed by 64 CAN-FD data bytes. Slot metadata carries sequence/time. */
static int poll_host_can(CAN_MSG *message)
{
    qv_ring_slot_t slot;
    int status = qv_client_ring_pop(&g_can.client, QV_RING_CAN_RX, &slot);
    if (status != EOK) {
        return status;
    }
    if (slot.type != QV_SLOT_TYPE_CAN ||
        (slot.payload[5] & QV_CAN_WIRE_FLAG_FD) == 0 ||
        !can_fd_length_valid(slot.payload[4])) {
        (void)qv_client_counter_add(&g_can.client, QV_COUNTER_QNX_CAN_RX, 1);
        ++g_can.dropped_count;
        return EPROTO;
    }
    slot_to_can_msg(&slot, message);
    ++g_can.rx_count;
    (void)qv_client_counter_add(&g_can.client, QV_COUNTER_QNX_CAN_RX, 1);
    return EOK;
}

static void *can_rx_worker(void *unused)
{
    (void)unused;
    while (!g_can.rx_stop) {
        CAN_MSG message;
        int status = poll_host_can(&message);
        if (status == EOK) {
            pthread_mutex_lock(&g_rx_lock);
            if (g_rx_count == QV_CAN_RX_QUEUE_SIZE) {
                g_rx_tail = (g_rx_tail + 1u) % QV_CAN_RX_QUEUE_SIZE;
                --g_rx_count;
                ++g_can.dropped_count;
            }
            g_rx_queue[g_rx_head] = message;
            g_rx_head = (g_rx_head + 1u) % QV_CAN_RX_QUEUE_SIZE;
            ++g_rx_count;
            pthread_cond_signal(&g_rx_ready);
            pthread_mutex_unlock(&g_rx_lock);
            continue;
        }
        if (status != EAGAIN) {
            fprintf(stderr, "QNX-Quiver CAN: RX service error: %s\n",
                    strerror(status));
        }
        if (sleep_poll_interval() == -1 && errno != EINTR) {
            fprintf(stderr, "QNX-Quiver CAN: RX poll sleep: %s\n", strerror(errno));
            break;
        }
    }
    pthread_mutex_lock(&g_rx_lock);
    pthread_cond_broadcast(&g_rx_ready);
    pthread_mutex_unlock(&g_rx_lock);
    return NULL;
}

static int pop_rx_message(CAN_MSG *message, int blocking)
{
    int status = EOK;
    pthread_mutex_lock(&g_rx_lock);
    while (blocking && g_rx_count == 0 && !g_can.rx_stop) {
        status = pthread_cond_wait(&g_rx_ready, &g_rx_lock);
        if (status != 0) {
            pthread_mutex_unlock(&g_rx_lock);
            return status;
        }
    }
    if (g_rx_count == 0) {
        status = g_can.rx_stop ? ECANCELED : EAGAIN;
    } else {
        *message = g_rx_queue[g_rx_tail];
        g_rx_tail = (g_rx_tail + 1u) % QV_CAN_RX_QUEUE_SIZE;
        --g_rx_count;
    }
    pthread_mutex_unlock(&g_rx_lock);
    return status;
}

static void quiver_sigterm_handler(int signal_number)
{
    g_can.rx_stop = 1;
    if ((g_libcan_sigterm.sa_flags & SA_SIGINFO) != 0) {
        if (g_libcan_sigterm.sa_sigaction != NULL) {
            g_libcan_sigterm.sa_sigaction(signal_number, NULL, NULL);
        }
    } else if (g_libcan_sigterm.sa_handler != SIG_DFL &&
               g_libcan_sigterm.sa_handler != SIG_IGN &&
               g_libcan_sigterm.sa_handler != NULL) {
        g_libcan_sigterm.sa_handler(signal_number);
    }
}

static void *watch_libcan_sigterm(void *unused)
{
    (void)unused;
    while (!g_signal_watch_stop) {
        struct sigaction current;
        if (sigaction(SIGTERM, NULL, &current) == 0 &&
            (current.sa_handler != g_initial_sigterm.sa_handler ||
             ((current.sa_flags ^ g_initial_sigterm.sa_flags) & SA_SIGINFO) != 0)) {
            g_libcan_sigterm = current;
            current.sa_handler = quiver_sigterm_handler;
            if (sigaction(SIGTERM, &current, NULL) == 0) {
                g_signal_hooked = 1;
                return NULL;
            }
        }
        if (sleep_poll_interval() == -1 && errno != EINTR) {
            break;
        }
    }
    return NULL;
}

static void can_transmit(CANDEV *cdev)
{
    canmsg_t *element;
    (void)cdev;
    while ((element = canmsg_dequeue_element(g_can.tx.msg_queue)) != NULL) {
        qv_ring_slot_t slot;
        int status = can_msg_to_slot(&element->cmsg, &slot);
        if (status == EOK) {
            status = qv_client_ring_push(&g_can.client, QV_RING_CAN_TX, &slot);
        }
        if (status == EOK) {
            ++g_can.tx_count;
            (void)qv_client_counter_add(&g_can.client, QV_COUNTER_QNX_CAN_TX, 1);
        } else {
            ++g_can.dropped_count;
            fprintf(stderr, "QNX-Quiver CAN: transmit dropped: %s\n", strerror(status));
        }
        canmsg_queue_element(g_can.tx.free_queue, element);
    }
}

static int can_devctl(CANDEV *cdev, io_devctl_t *msg)
{
    if (cdev == &g_can.tx && msg->i.dcmd == CAN_DEVCTL_TX_FRAME_RAW) {
        const CAN_MSG *input = _IO_INPUT_PAYLOAD(msg);
        qv_ring_slot_t slot;
        int status;
        if (input == NULL) {
            errno = EFAULT;
            return -1;
        }
        status = can_msg_to_slot(input, &slot);
        if (status == EOK) {
            status = qv_client_ring_push(&g_can.client, QV_RING_CAN_TX, &slot);
        }
        if (status != EOK) {
            errno = status;
            return -1;
        }
        ++g_can.tx_count;
        (void)qv_client_counter_add(&g_can.client, QV_COUNTER_QNX_CAN_TX, 1);
        return EOK;
    }

    if (cdev == &g_can.rx &&
        (msg->i.dcmd == CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK ||
         msg->i.dcmd == CAN_DEVCTL_RX_FRAME_RAW_BLOCK)) {
        CAN_MSG *out = _IO_OUTPUT_PAYLOAD(msg);
        if (out == NULL) {
            errno = EFAULT;
            return -1;
        }
        int status = pop_rx_message(
            out, msg->i.dcmd == CAN_DEVCTL_RX_FRAME_RAW_BLOCK);
        if (status != EOK) {
            errno = status;
            return -1;
        }
        return sizeof(*out);
    }

    errno = ENOTSUP;
    return -1;
}

static CANDEV *can_event_handler(void *const handle, const int code)
{
    (void)handle;
    (void)code;
    return NULL;
}

static int init_can_device(CANDEV *device, CANDEV_TYPE type, int unit)
{
    CANDEV_INIT init = {
        .devtype = type,
        .can_unit = QV_CAN_UNIT,
        .dev_unit = unit,
        .msgq_size = QV_CAN_QUEUE_SIZE,
        .waitq_size = QV_CAN_QUEUE_SIZE,
        .mode = CANDEV_MODE_RAW_FRAME
    };
    int status = can_resmgr_init_device(device, &init);
    if (status == -1) {
        return errno;
    }
    if (status == EOK) {
        device->cflags = CANDEV_CFLAG_CANFD;
    }
    return status;
}

static void destroy_can(void)
{
    g_signal_watch_stop = 1;
    if (g_signal_watch_started) {
        (void)pthread_join(g_can.signal_watcher, NULL);
        g_signal_watch_started = 0;
    }
    if (g_signal_hooked) {
        (void)sigaction(SIGTERM, &g_initial_sigterm, NULL);
        g_signal_hooked = 0;
    }
    if (g_can.initialized) {
        if (g_can.rx_worker_started) {
            g_can.rx_stop = 1;
            pthread_mutex_lock(&g_rx_lock);
            pthread_cond_broadcast(&g_rx_ready);
            pthread_mutex_unlock(&g_rx_lock);
            (void)pthread_join(g_can.rx_worker, NULL);
            g_can.rx_worker_started = 0;
        }
        if (g_can.tx_initialized) {
            can_resmgr_destroy_device(&g_can.tx);
            g_can.tx_initialized = 0;
            g_can.tx_created = 0;
        }
        if (g_can.rx_initialized) {
            can_resmgr_destroy_device(&g_can.rx);
            g_can.rx_initialized = 0;
            g_can.rx_created = 0;
        }
        can_resmgr_fini();
        g_can.initialized = 0;
    }
    qv_client_close(&g_can.client);
}

int main(int argc, char **argv)
{
    const int rx_enabled = argc == 2 && strcmp(argv[1], "rx") == 0;
    const int tx_enabled = argc == 2 && strcmp(argv[1], "tx") == 0;
    can_drvr_funcs_t callbacks = {
        .transmit = can_transmit,
        .devctl = can_devctl,
        .transmit_raw = NULL,
        .receive_raw = NULL,
        .event_handler = can_event_handler
    };
    if (!rx_enabled && !tx_enabled) {
        fprintf(stderr, "usage: %s rx|tx (run one libcan endpoint per process)\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    const char *stage = "open /dev/quiver";
    int status = qv_client_open(&g_can.client, QV_CLIENT_DEVICE_PATH, O_RDWR);
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver CAN: open %s: %s\n",
                QV_CLIENT_DEVICE_PATH, strerror(status));
        return EXIT_FAILURE;
    }

    stage = "can_resmgr_init";
    status = can_resmgr_init(&callbacks, 6);
    if (status == -1) {
        status = errno;
    }
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver CAN: can_resmgr_init: %s\n", strerror(status));
        destroy_can();
        return EXIT_FAILURE;
    }
    g_can.initialized = 1;
    stage = "initialize TX device";
    status = tx_enabled ? init_can_device(&g_can.tx, CANDEV_TYPE_TX, QV_CAN_TX_UNIT) : EOK;
    g_can.tx_initialized = tx_enabled && status == EOK;
    if (status == EOK && rx_enabled) {
        stage = "initialize RX device";
        status = init_can_device(&g_can.rx, CANDEV_TYPE_RX, QV_CAN_RX_UNIT);
        g_can.rx_initialized = status == EOK;
    }
    if (status == EOK && tx_enabled) {
        stage = "publish TX device node";
        status = can_resmgr_create_device(&g_can.tx);
        if (status == -1) status = errno;
        g_can.tx_created = status == EOK;
    }
    if (status == EOK && rx_enabled) {
        stage = "publish RX device node";
        status = can_resmgr_create_device(&g_can.rx);
        if (status == -1) status = errno;
        g_can.rx_created = status == EOK;
    }
    if (status != EOK) {
        fprintf(stderr, "QNX-Quiver CAN: %s: %s (%d)\n", stage,
                strerror(status), status);
        destroy_can();
        return EXIT_FAILURE;
    }

    if (rx_enabled) {
        status = pthread_create(&g_can.rx_worker, NULL, can_rx_worker, NULL);
        if (status != 0) {
            fprintf(stderr, "QNX-Quiver CAN: start RX worker: %s\n", strerror(status));
            destroy_can();
            return EXIT_FAILURE;
        }
        g_can.rx_worker_started = 1;
        if (sigaction(SIGTERM, NULL, &g_initial_sigterm) == -1) {
            fprintf(stderr, "QNX-Quiver CAN: inspect SIGTERM handler: %s\n",
                    strerror(errno));
            destroy_can();
            return EXIT_FAILURE;
        }
        status = pthread_create(&g_can.signal_watcher, NULL,
                                watch_libcan_sigterm, NULL);
        if (status != 0) {
            fprintf(stderr, "QNX-Quiver CAN: start signal watcher: %s\n",
                    strerror(status));
            destroy_can();
            return EXIT_FAILURE;
        }
        g_signal_watch_started = 1;
    }

    fprintf(stdout, "QNX-Quiver CAN online:%s%s\n",
            rx_enabled ? " /dev/can0/rx0" : "",
            tx_enabled ? " /dev/can0/tx0" : "");
    can_resmgr_start();
    destroy_can();
    fprintf(stdout, "QNX-Quiver CAN stopped: rx=%llu tx=%llu dropped=%llu\n",
            (unsigned long long)g_can.rx_count,
            (unsigned long long)g_can.tx_count,
            (unsigned long long)g_can.dropped_count);
    return EXIT_SUCCESS;
}
