#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>

#include <devctl.h>
#include <pci/pci.h>

#include <sys/iofunc.h>
#include <sys/resmgr.h>
#include <sys/dispatch.h>
#include <secpol/secpol.h>

#include "quiver_protocol.h"
#include "quiver_client.h"

#define QUIVER_PCI_VENDOR 0x1af4u
#define QUIVER_PCI_DEVICE 0x1110u
#define QUIVER_BAR2_NUMBER 2
#define QUIVER_DEVICE_PATH "/dev/quiver"
#define QUIVER_STATUS_CAPACITY 192u

typedef struct {
    pci_devhdl_t handle;
    void *bar2_mapping;
    pci_bdf_t known_bdf;
    int known_bdf_valid;
    int recovery_diagnostic_reported;
} quiver_pci_t;

typedef struct {
    iofunc_attr_t attr;
    char status[QUIVER_STATUS_CAPACITY];
    size_t status_bytes;
    qv_bar2_t *bar2;
    int online;
} quiver_device_t;

static int release_pci(quiver_pci_t *pci);

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t health_check_due;

static int health_timeout_handler(message_context_t *ctp, int code,
                                  unsigned flags, void *handle)
{
    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;
    health_check_due = 1;
    return 0;
}

static void request_shutdown(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int install_shutdown_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_shutdown;
    if (sigemptyset(&action.sa_mask) == -1) {
        return -1;
    }
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGHUP, &action, NULL) == -1) {
        return -1;
    }
    return 0;
}

static int attach_and_map(quiver_pci_t *pci, uint32_t *magic_out,
                          uint16_t *version_out, uint32_t *bytes_out,
                          uint64_t *bar_address_out, int quiet)
{
    pci_err_t error = PCI_ERR_OK;
    pci_bdf_t bdf;
    pci_ba_t bars[6] = {{0}};
    int bar_count = (int)(sizeof(bars) / sizeof(bars[0]));
    pci_ba_t *bar2 = NULL;
    uint16_t superblock_bytes;
    pci_err_t known_vid_error = PCI_ERR_OK;
    pci_err_t known_did_error = PCI_ERR_OK;
    pci_vid_t known_vendor = PCI_VID_ANY;
    pci_did_t known_device = PCI_DID_ANY;
    int known_bdf_matches = 0;

    if (ThreadCtl(_NTO_TCTL_IO, 0) == -1) {
        if (!quiet) {
            fprintf(stderr, "QNX-Quiver: ThreadCtl(_NTO_TCTL_IO): %s\n",
                    strerror(errno));
        }
        return -1;
    }

    bdf = PCI_BDF_NONE;
    if (pci->known_bdf_valid) {
        known_vid_error = pci_device_read_vid(pci->known_bdf, &known_vendor);
        if (known_vid_error == PCI_ERR_OK &&
            known_vendor == QUIVER_PCI_VENDOR) {
            known_did_error = pci_device_read_did(pci->known_bdf, &known_device);
            known_bdf_matches = known_did_error == PCI_ERR_OK &&
                                known_device == QUIVER_PCI_DEVICE;
        }
        if (known_bdf_matches) {
            bdf = pci->known_bdf;
        }
    }
    if (bdf == PCI_BDF_NONE) {
        bdf = pci_device_find(0, QUIVER_PCI_VENDOR, QUIVER_PCI_DEVICE,
                              PCI_CCODE_ANY);
    }
    if (bdf == PCI_BDF_NONE) {
        if (quiet && pci->known_bdf_valid &&
            !pci->recovery_diagnostic_reported) {
            fprintf(stderr,
                    "QNX-Quiver: recovery probe at B%u:D%u:F%u: "
                    "VID %04x (%s), DID %04x (%s); PCI database has no match\n",
                    PCI_BUS(pci->known_bdf), PCI_DEV(pci->known_bdf),
                    PCI_FUNC(pci->known_bdf), known_vendor,
                    pci_strerror(known_vid_error), known_device,
                    pci_strerror(known_did_error));
            pci->recovery_diagnostic_reported = 1;
        }
        if (!quiet) {
            fprintf(stderr, "QNX-Quiver: PCI device %04x:%04x not found\n",
                    QUIVER_PCI_VENDOR, QUIVER_PCI_DEVICE);
        }
        return -1;
    }
    pci->known_bdf = bdf;
    pci->known_bdf_valid = 1;

    pci->handle = pci_device_attach(bdf, pci_attachFlags_EXCLUSIVE_OWNER, &error);
    if (pci->handle == NULL) {
        if (!quiet || (pci->known_bdf_valid &&
                       !pci->recovery_diagnostic_reported)) {
            fprintf(stderr, "QNX-Quiver: pci_device_attach: %s (%d)\n",
                    pci_strerror(error), error);
            pci->recovery_diagnostic_reported = 1;
        }
        return -1;
    }
    pci->recovery_diagnostic_reported = 0;

    error = pci_device_read_ba(pci->handle, &bar_count, bars,
                               pci_reqType_e_UNSPECIFIED);
    if (error != PCI_ERR_OK) {
        if (!quiet || (pci->known_bdf_valid &&
                       !pci->recovery_diagnostic_reported)) {
            fprintf(stderr, "QNX-Quiver: pci_device_read_ba: %s (%d)\n",
                    pci_strerror(error), error);
            pci->recovery_diagnostic_reported = 1;
        }
        return -1;
    }
    if (bar_count < 0 || bar_count > (int)(sizeof(bars) / sizeof(bars[0]))) {
        if (!quiet || (pci->known_bdf_valid &&
                       !pci->recovery_diagnostic_reported)) {
            fprintf(stderr, "QNX-Quiver: invalid BAR count: %d\n", bar_count);
            pci->recovery_diagnostic_reported = 1;
        }
        return -1;
    }
    for (int index = 0; index < bar_count; ++index) {
        if (bars[index].bar_num == QUIVER_BAR2_NUMBER) {
            bar2 = &bars[index];
            break;
        }
    }
    if (bar2 == NULL || bar2->type != pci_asType_e_MEM ||
        bar2->size < QV_BAR2_BYTES) {
        if (!quiet || (pci->known_bdf_valid &&
                       !pci->recovery_diagnostic_reported)) {
            fprintf(stderr, "QNX-Quiver: BAR2 is absent, not memory, or smaller than %u bytes\n",
                    QV_BAR2_BYTES);
            pci->recovery_diagnostic_reported = 1;
        }
        return -1;
    }

    pci->bar2_mapping = mmap_device_memory(NULL, QV_BAR2_BYTES,
                                           PROT_READ | PROT_WRITE | PROT_NOCACHE, 0,
                                           (uint64_t)bar2->addr);
    if (pci->bar2_mapping == MAP_FAILED) {
        pci->bar2_mapping = NULL;
        if (!quiet) {
            fprintf(stderr, "QNX-Quiver: map BAR2: %s\n", strerror(errno));
        }
        return -1;
    }

    memcpy(magic_out, (const uint8_t *)pci->bar2_mapping, sizeof(*magic_out));
    memcpy(version_out, (const uint8_t *)pci->bar2_mapping + 4,
           sizeof(*version_out));
    memcpy(&superblock_bytes, (const uint8_t *)pci->bar2_mapping + 6,
           sizeof(superblock_bytes));
    memcpy(bytes_out, (const uint8_t *)pci->bar2_mapping + 8,
           sizeof(*bytes_out));
    if (*magic_out != QV_PROTOCOL_MAGIC ||
        *version_out != QV_PROTOCOL_VERSION ||
        superblock_bytes != sizeof(qv_superblock_t) ||
        *bytes_out != QV_BAR2_BYTES) {
        if (!quiet) {
            fprintf(stderr,
                    "QNX-Quiver: protocol mismatch: magic=0x%08" PRIx32
                    " version=%u header=%u bytes=%" PRIu32 "\n",
                    *magic_out, *version_out, superblock_bytes, *bytes_out);
        }
        return -1;
    }
    *bar_address_out = (uint64_t)bar2->addr;
    return 0;
}

static int publish_node(dispatch_t *dispatch, int *attach_id,
                        quiver_device_t *device,
    resmgr_attr_t *resource_attributes,
                        const resmgr_connect_funcs_t *connect_functions,
                        const resmgr_io_funcs_t *io_functions,
                        uint32_t magic, uint16_t version,
                        uint32_t shared_bytes, uint64_t bar2_address,
                        void *bar2_mapping)
{
    memset(device, 0, sizeof(*device));
    device->status_bytes = (size_t)snprintf(
        device->status, sizeof(device->status),
        "QNX-Quiver PCI service online\n"
        "device=%04x:%04x\nBAR2=0x%" PRIx64 " size=%" PRIu32 "\n"
        "protocol=QVB1 version=%u magic=0x%08" PRIx32 "\n",
        QUIVER_PCI_VENDOR, QUIVER_PCI_DEVICE, bar2_address,
        shared_bytes, version, magic);
    if (device->status_bytes >= sizeof(device->status)) {
        fprintf(stderr, "QNX-Quiver: status response exceeded its buffer\n");
        return -1;
    }
    iofunc_attr_init(&device->attr, S_IFNAM | 0660, NULL, NULL);
    device->bar2 = (qv_bar2_t *)bar2_mapping;
    device->online = 1;
    device->attr.nbytes = (off_t)device->status_bytes;
    *attach_id = secpol_resmgr_attach(NULL, dispatch, resource_attributes,
                                      QUIVER_DEVICE_PATH, _FTYPE_ANY, 0,
                                      connect_functions, io_functions,
                                      &device->attr, NULL);
    if (*attach_id == -1) {
        perror("QNX-Quiver: resmgr_attach /dev/quiver");
        return -1;
    }
    return 0;
}

static void withdraw_node(dispatch_t *dispatch, int *attach_id)
{
    if (*attach_id != -1) {
        if (resmgr_detach(dispatch, *attach_id, _RESMGR_DETACH_ALL) == -1) {
            perror("QNX-Quiver: resmgr_detach /dev/quiver");
        }
        *attach_id = -1;
    }
}

static qv_ring_region_t *ring_for_id(qv_bar2_t *bar2, uint32_t ring_id)
{
    switch (ring_id) {
    case QV_RING_CAN_RX: return &bar2->can_rx;
    case QV_RING_CAN_TX: return &bar2->can_tx;
    case QV_RING_IMU_RX: return &bar2->imu_rx;
    case QV_RING_ACTUATOR_TX: return &bar2->actuator_tx;
    default: return NULL;
    }
}

static int ring_layout_valid(const qv_ring_region_t *ring)
{
    return ring->control.slot_count == QV_RING_SLOTS &&
           ring->control.slot_bytes == QV_RING_SLOT_BYTES;
}

static int ring_pop(qv_ring_region_t *ring, qv_ring_slot_t *slot)
{
    if (!ring_layout_valid(ring)) {
        return EPROTO;
    }
    uint32_t head = atomic_load_explicit(&ring->control.head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&ring->control.tail, memory_order_relaxed);
    if (head >= QV_RING_SLOTS || tail >= QV_RING_SLOTS) {
        return EPROTO;
    }
    if (tail == head) {
        return EAGAIN;
    }
    *slot = ring->slots[tail];
    atomic_store_explicit(&ring->control.tail, (tail + 1u) % QV_RING_SLOTS,
                          memory_order_release);
    return EOK;
}

static int ring_push(qv_ring_region_t *ring, const qv_ring_slot_t *slot)
{
    if (!ring_layout_valid(ring)) {
        return EPROTO;
    }
    uint32_t head = atomic_load_explicit(&ring->control.head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&ring->control.tail, memory_order_acquire);
    if (head >= QV_RING_SLOTS || tail >= QV_RING_SLOTS) {
        return EPROTO;
    }
    uint32_t next_head = (head + 1u) % QV_RING_SLOTS;
    if (next_head == tail) {
        (void)atomic_fetch_add_explicit(&ring->control.dropped, 1u,
                                        memory_order_relaxed);
        return EAGAIN;
    }
    ring->slots[head] = *slot;
    atomic_store_explicit(&ring->control.head, next_head, memory_order_release);
    return EOK;
}

static void fault_write_begin(qv_fault_bank_t *faults)
{
    uint32_t sequence = atomic_load_explicit(&faults->qnx_seq,
                                              memory_order_relaxed);
    if ((sequence & 1u) != 0u) ++sequence; /* Recover a prior manager exit. */
    atomic_store_explicit(&faults->qnx_seq, sequence + 1u,
                          memory_order_release);
}

static void fault_write_end(qv_fault_bank_t *faults)
{
    uint32_t sequence = atomic_load_explicit(&faults->qnx_seq,
                                              memory_order_relaxed);
    atomic_store_explicit(&faults->qnx_seq, sequence + 1u,
                          memory_order_release);
}

static int fault_snapshot(qv_fault_bank_t *faults,
                          qv_client_faults_t *result_out)
{
    uint32_t host_before, qnx_before, host_after, qnx_after;
    for (unsigned attempt = 0; attempt < 10000u; ++attempt) {
        host_before = atomic_load_explicit(&faults->host_seq,
                                           memory_order_acquire);
        qnx_before = atomic_load_explicit(&faults->qnx_seq,
                                          memory_order_acquire);
        if (((host_before | qnx_before) & 1u) != 0u) {
            (void)sched_yield();
            continue;
        }
        result_out->fault_flags = atomic_load_explicit(&faults->fault_flags,
                                                   memory_order_relaxed);
        result_out->latency_spike_us = atomic_load_explicit(&faults->latency_spike_us,
                                                        memory_order_relaxed);
        result_out->host_can_tx = atomic_load_explicit(&faults->host_can_tx,
                                                   memory_order_relaxed);
        result_out->host_can_rx = atomic_load_explicit(&faults->host_can_rx,
                                                   memory_order_relaxed);
        result_out->qnx_can_tx = atomic_load_explicit(&faults->qnx_can_tx,
                                                  memory_order_relaxed);
        result_out->qnx_can_rx = atomic_load_explicit(&faults->qnx_can_rx,
                                                  memory_order_relaxed);
        result_out->dropped_frames = atomic_load_explicit(&faults->dropped_frames,
                                                      memory_order_relaxed);
        result_out->recovery_count = atomic_load_explicit(&faults->recovery_count,
                                                      memory_order_relaxed);
        result_out->qnx_can_dropped = atomic_load_explicit(&faults->qnx_can_dropped,
                                                       memory_order_relaxed);
        result_out->host_actuator_accepted = atomic_load_explicit(
            &faults->host_actuator_accepted, memory_order_relaxed);
        result_out->host_actuator_rejected = atomic_load_explicit(
            &faults->host_actuator_rejected, memory_order_relaxed);
        result_out->host_imu_dropped = atomic_load_explicit(
            &faults->host_imu_dropped, memory_order_relaxed);
        result_out->host_actuator_last_id = atomic_load_explicit(
            &faults->host_actuator_last_id, memory_order_relaxed);
        result_out->host_actuator_last_rejected_id = atomic_load_explicit(
            &faults->host_actuator_last_rejected_id, memory_order_relaxed);
        uint32_t actuator_output_bits = atomic_load_explicit(
            &faults->host_actuator_output_bits, memory_order_relaxed);
        memcpy(&result_out->host_actuator_output, &actuator_output_bits,
               sizeof(result_out->host_actuator_output));
        result_out->qnx_imu_dropped = atomic_load_explicit(
            &faults->qnx_imu_dropped, memory_order_relaxed);
        result_out->qnx_actuator_dropped = atomic_load_explicit(
            &faults->qnx_actuator_dropped, memory_order_relaxed);
        host_after = atomic_load_explicit(&faults->host_seq,
                                          memory_order_acquire);
        qnx_after = atomic_load_explicit(&faults->qnx_seq,
                                         memory_order_acquire);
        if (host_before == host_after && qnx_before == qnx_after &&
            ((host_after | qnx_after) & 1u) == 0u) {
            result_out->host_generation = host_after;
            result_out->qnx_generation = qnx_after;
            return EOK;
        }
        if ((attempt & 0xffu) == 0u) (void)sched_yield();
    }
    return EAGAIN;
}

static int quiver_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                         iofunc_ocb_t *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) {
        return status;
    }
    quiver_device_t *device = (quiver_device_t *)ocb->attr;
    if (!device->online || device->bar2 == NULL) {
        return ENODEV;
    }
    size_t input_bytes = ctp->size >= sizeof(msg->i)
                             ? ctp->size - sizeof(msg->i)
                             : 0;
    size_t output_capacity = ctp->info.dstmsglen > 0
                                 ? (size_t)ctp->info.dstmsglen
                                 : 0;
    struct _io_devctl_reply *reply = (struct _io_devctl_reply *)ctp->msg;
    size_t output_bytes = 0;
    void *output = _IO_OUTPUT_PAYLOAD(msg);

    switch (msg->i.dcmd) {
    case QV_DCMD_GET_INFO: {
        if ((ocb->ioflag & (_IO_FLAG_RD | _IO_FLAG_WR)) == 0) return EACCES;
        if (output_capacity < sizeof(*reply) + sizeof(qv_client_info_t)) {
            return EINVAL;
        }
        qv_client_info_t result = {0};
        result.api_version = QV_CLIENT_API_VERSION;
        result.protocol_magic = device->bar2->superblock.magic;
        result.protocol_version = device->bar2->superblock.protocol_version;
        result.bar_bytes = device->bar2->superblock.shared_bytes;
        result.capabilities = device->bar2->superblock.capabilities;
        result.generation = atomic_load_explicit(&device->bar2->superblock.generation,
                                                 memory_order_acquire);
        result.ring_count = QV_RING_COUNT;
        result.ring_slots = QV_RING_SLOTS;
        result.ring_capacity = QV_RING_CAPACITY;
        result.ring_slot_bytes = QV_RING_SLOT_BYTES;
        memcpy(output, &result, sizeof(result));
        output_bytes = sizeof(result);
        break;
    }
    case QV_DCMD_RING_POP:
    case QV_DCMD_RING_PUSH: {
        const int is_pop = msg->i.dcmd == QV_DCMD_RING_POP;
        const size_t required = sizeof(qv_client_ring_io_t);
        if (input_bytes < required) return EBADMSG;
        if (is_pop && output_capacity < sizeof(*reply) + required) return EINVAL;
        if (is_pop ? (ocb->ioflag & _IO_FLAG_RD) == 0
                   : (ocb->ioflag & _IO_FLAG_WR) == 0) return EACCES;

        qv_client_ring_io_t request;
        memcpy(&request, _IO_INPUT_PAYLOAD(msg), sizeof(request));
        if (request.reserved != 0) return EINVAL;
        qv_ring_region_t *ring = ring_for_id(device->bar2, request.ring_id);
        if (ring == NULL) return EINVAL;
        if (is_pop) {
            if (request.ring_id != QV_RING_CAN_RX &&
                request.ring_id != QV_RING_IMU_RX) return EACCES;
            status = ring_pop(ring, &request.slot);
            if (status != EOK) return status;
            if ((request.ring_id == QV_RING_CAN_RX &&
                 request.slot.type != QV_SLOT_TYPE_CAN) ||
                (request.ring_id == QV_RING_IMU_RX &&
                 request.slot.type != QV_SLOT_TYPE_IMU)) return EPROTO;
            memcpy(output, &request, sizeof(request));
            output_bytes = sizeof(request);
        } else {
            if (request.ring_id != QV_RING_CAN_TX &&
                request.ring_id != QV_RING_ACTUATOR_TX) return EACCES;
            if ((request.ring_id == QV_RING_CAN_TX &&
                 request.slot.type != QV_SLOT_TYPE_CAN) ||
                (request.ring_id == QV_RING_ACTUATOR_TX &&
                 request.slot.type != QV_SLOT_TYPE_ACTUATOR)) return EINVAL;
            status = ring_push(ring, &request.slot);
            if (status != EOK) return status;
        }
        break;
    }
    case QV_DCMD_GET_FAULTS: {
        if ((ocb->ioflag & _IO_FLAG_RD) == 0) return EACCES;
        if (output_capacity < sizeof(*reply) + sizeof(qv_client_faults_t)) {
            return EINVAL;
        }
        qv_client_faults_t result = {0};
        status = fault_snapshot(&device->bar2->faults, &result);
        if (status != EOK) return status;
        memcpy(output, &result, sizeof(result));
        output_bytes = sizeof(result);
        break;
    }
    case QV_DCMD_COUNTER_ADD: {
        if ((ocb->ioflag & _IO_FLAG_WR) == 0) return EACCES;
        if (input_bytes < sizeof(qv_client_counter_add_t)) return EBADMSG;
        qv_client_counter_add_t request;
        memcpy(&request, _IO_INPUT_PAYLOAD(msg), sizeof(request));
        if (request.reserved != 0 || request.amount == 0) return EINVAL;
        _Atomic uint64_t *counter = NULL;
        switch (request.counter) {
        case QV_COUNTER_QNX_CAN_TX: counter = &device->bar2->faults.qnx_can_tx; break;
        case QV_COUNTER_QNX_CAN_RX: counter = &device->bar2->faults.qnx_can_rx; break;
        case QV_COUNTER_RECOVERY: counter = &device->bar2->faults.recovery_count; break;
        case QV_COUNTER_QNX_CAN_DROPPED: counter = &device->bar2->faults.qnx_can_dropped; break;
        case QV_COUNTER_QNX_IMU_DROPPED: counter = &device->bar2->faults.qnx_imu_dropped; break;
        case QV_COUNTER_QNX_ACTUATOR_DROPPED: counter = &device->bar2->faults.qnx_actuator_dropped; break;
        default: return EINVAL;
        }
        fault_write_begin(&device->bar2->faults);
        (void)atomic_fetch_add_explicit(counter, request.amount, memory_order_relaxed);
        fault_write_end(&device->bar2->faults);
        break;
    }
    default:
        return ENOSYS;
    }

    memset(reply, 0, sizeof(*reply));
    reply->nbytes = (uint32_t)output_bytes;
    SETIOV(ctp->iov, reply, sizeof(*reply) + output_bytes);
    return _RESMGR_NPARTS(1);
}

static void check_device_health(dispatch_t *dispatch, int *attach_id,
                                quiver_device_t *device, quiver_pci_t *pci,
                                resmgr_attr_t *resource_attributes,
                                const resmgr_connect_funcs_t *connect_functions,
                                const resmgr_io_funcs_t *io_functions,
                                int *unavailable_reported)
{
    if (pci->handle != NULL) {
        pci_ba_t bars[6] = {{0}};
        int bar_count = (int)(sizeof(bars) / sizeof(bars[0]));
        pci_err_t error = pci_device_read_ba(pci->handle, &bar_count, bars,
                                              pci_reqType_e_UNSPECIFIED);
        int bar2_valid = 0;
        if (error == PCI_ERR_OK && bar_count >= 0 &&
            bar_count <= (int)(sizeof(bars) / sizeof(bars[0]))) {
            for (int index = 0; index < bar_count; ++index) {
                if (bars[index].bar_num == QUIVER_BAR2_NUMBER &&
                    bars[index].type == pci_asType_e_MEM &&
                    bars[index].size >= QV_BAR2_BYTES) {
                    bar2_valid = 1;
                    break;
                }
            }
        }
        if (bar2_valid) {
            return;
        }

        fprintf(stderr, "QNX-Quiver: PCI device became unavailable (%s)\n",
                error == PCI_ERR_OK ? "BAR2 no longer valid" : pci_strerror(error));
        device->online = 0;
        device->bar2 = NULL;
        withdraw_node(dispatch, attach_id);
        if (error == PCI_ERR_SERVER_NOT_AVAILABLE) {
            /* QNX requires abandoning old handles after an async server loss. */
            if (pci->bar2_mapping != NULL &&
                munmap(pci->bar2_mapping, QV_BAR2_BYTES) == -1) {
                fprintf(stderr, "QNX-Quiver: unmap BAR2 after server loss: %s\n",
                        strerror(errno));
            }
            pci->bar2_mapping = NULL;
            pci->handle = NULL;
        } else {
            (void)release_pci(pci);
        }
        *unavailable_reported = 1;
        return;
    }

    uint32_t magic = 0;
    uint16_t version = 0;
    uint32_t shared_bytes = 0;
    uint64_t bar2_address = 0;
    if (attach_and_map(pci, &magic, &version, &shared_bytes,
                       &bar2_address, 1) == -1) {
        (void)release_pci(pci);
        if (!*unavailable_reported) {
            fprintf(stderr, "QNX-Quiver: waiting for PCI device/server recovery\n");
            *unavailable_reported = 1;
        }
        return;
    }
    if (publish_node(dispatch, attach_id, device, resource_attributes,
                     connect_functions, io_functions, magic, version,
                     shared_bytes, bar2_address, pci->bar2_mapping) == -1) {
        (void)release_pci(pci);
        return;
    }
    printf("QNX-Quiver: PCI device recovered; %s is available\n",
           QUIVER_DEVICE_PATH);
    fflush(stdout);
    *unavailable_reported = 0;
}

static int release_pci(quiver_pci_t *pci)
{
    int result = EXIT_SUCCESS;
    if (pci->bar2_mapping != NULL) {
        if (munmap(pci->bar2_mapping, QV_BAR2_BYTES) == -1) {
            fprintf(stderr, "QNX-Quiver: unmap BAR2: %s\n", strerror(errno));
            result = EXIT_FAILURE;
        }
        pci->bar2_mapping = NULL;
    }
    if (pci->handle != NULL) {
        pci_err_t error = pci_device_detach(pci->handle);
        if (error != PCI_ERR_OK) {
            fprintf(stderr, "QNX-Quiver: pci_device_detach: %s (%d)\n",
                    pci_strerror(error), error);
            result = EXIT_FAILURE;
        }
        pci->handle = NULL;
    }
    return result;
}

static int quiver_read(resmgr_context_t *ctp, io_read_t *msg,
                       iofunc_ocb_t *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    if (ocb->offset < 0) {
        return EINVAL;
    }

    quiver_device_t *device = (quiver_device_t *)ocb->attr;
    if (!device->online || device->bar2 == NULL) {
        return ENODEV;
    }
    size_t offset = (size_t)ocb->offset;
    size_t requested = (size_t)_IO_READ_GET_NBYTES(msg);
    size_t available = offset < device->status_bytes
                           ? device->status_bytes - offset
                           : 0;
    size_t count = requested < available ? requested : available;

    if (count == 0) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    SETIOV(ctp->iov, device->status + offset, count);
    _IO_SET_READ_NBYTES(ctp, count);
    ocb->offset += (off_t)count;
    device->attr.flags |= IOFUNC_ATTR_ATIME;
    return _RESMGR_NPARTS(1);
}

int main(void)
{
    dispatch_t *dispatch = NULL;
    dispatch_context_t *context = NULL;
    int attach_id = -1;
    int exit_status = EXIT_FAILURE;
    int unavailable_reported = 0;
    quiver_pci_t pci = {0};
    quiver_device_t device;
    resmgr_attr_t resource_attributes;
    resmgr_connect_funcs_t connect_functions;
    resmgr_io_funcs_t io_functions;
    struct timespec health_interval = {1, 0};
    uint32_t magic = 0;
    uint16_t version = 0;
    uint32_t shared_bytes = 0;
    uint64_t bar2_address = 0;

    memset(&device, 0, sizeof(device));
    if (install_shutdown_signals() == -1) {
        fprintf(stderr, "QNX-Quiver: signal setup: %s\n", strerror(errno));
        goto cleanup;
    }
    if ((dispatch = dispatch_create()) == NULL) {
        perror("QNX-Quiver: dispatch_create");
        goto cleanup;
    }

    message_attr_t timeout_attributes = {.flags = MSG_FLAG_TIMEOUT_FUNC};
    if (message_attach(dispatch, &timeout_attributes, 0, 0,
                       health_timeout_handler, NULL) == -1) {
        perror("QNX-Quiver: message_attach timeout handler");
        goto cleanup;
    }

    memset(&resource_attributes, 0, sizeof(resource_attributes));
    resource_attributes.nparts_max = 1;
    resource_attributes.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_functions,
                     _RESMGR_IO_NFUNCS, &io_functions);
    io_functions.read = quiver_read;
    io_functions.devctl = quiver_devctl;
    if (attach_and_map(&pci, &magic, &version, &shared_bytes,
                       &bar2_address, 1) == -1) {
        /*
         * Remain resident when the device is absent at startup. This is
         * required for a later PCI insertion to recover without restarting
         * the manager. The public node stays withdrawn until BAR2 and the
         * protocol have both been validated.
         */
        (void)release_pci(&pci);
        fprintf(stderr,
                "QNX-Quiver: starting offline; waiting for PCI device/server recovery\n");
        unavailable_reported = 1;
    } else {
        if (publish_node(dispatch, &attach_id, &device, &resource_attributes,
                         &connect_functions, &io_functions, magic, version,
                         shared_bytes, bar2_address, pci.bar2_mapping) == -1) {
            goto cleanup;
        }
        printf("QNX-Quiver: resident at %s; BAR2=0x%" PRIx64
               " (%" PRIu32 " bytes), QVB1 v%u\n",
               QUIVER_DEVICE_PATH, bar2_address, shared_bytes, version);
        fflush(stdout);
    }
    if (stopping) {
        exit_status = EXIT_SUCCESS;
        goto cleanup;
    }

    context = dispatch_context_alloc(dispatch);
    if (context == NULL) {
        perror("QNX-Quiver: dispatch_context_alloc");
        goto cleanup;
    }

    exit_status = EXIT_SUCCESS;

    while (!stopping) {
        if (dispatch_timeout(dispatch, &health_interval) == -1) {
            perror("QNX-Quiver: dispatch_timeout");
            exit_status = EXIT_FAILURE;
            break;
        }
        context = dispatch_block(context);
        if (context == NULL) {
            if (stopping && errno == EINTR) {
                break;
            }
            perror("QNX-Quiver: dispatch_block");
            exit_status = EXIT_FAILURE;
            break;
        }
        if (dispatch_handler(context) == -1) {
            if (errno == ETIMEDOUT) {
                health_check_due = 0;
                if (!stopping) {
                    check_device_health(dispatch, &attach_id, &device, &pci,
                                        &resource_attributes, &connect_functions,
                                        &io_functions, &unavailable_reported);
                }
                continue;
            }
            perror("QNX-Quiver: dispatch_handler");
            exit_status = EXIT_FAILURE;
            break;
        }
        if (health_check_due) {
            health_check_due = 0;
            if (!stopping) {
                check_device_health(dispatch, &attach_id, &device, &pci,
                                    &resource_attributes, &connect_functions,
                                    &io_functions, &unavailable_reported);
            }
        }
    }

cleanup:
    device.online = 0;
    device.bar2 = NULL;
    if (dispatch != NULL) {
        withdraw_node(dispatch, &attach_id);
    }
    if (context != NULL && dispatch != NULL) {
        dispatch_context_free(context);
    }
    if (attach_id != -1 && dispatch != NULL) {
        if (resmgr_detach(dispatch, attach_id, _RESMGR_DETACH_ALL) == -1) {
            perror("QNX-Quiver: resmgr_detach");
            exit_status = EXIT_FAILURE;
        }
    }
    if (dispatch != NULL && dispatch_destroy(dispatch) == -1) {
        perror("QNX-Quiver: dispatch_destroy");
        exit_status = EXIT_FAILURE;
    }
    if (release_pci(&pci) == EXIT_FAILURE) {
        exit_status = EXIT_FAILURE;
    }
    return exit_status;
}
