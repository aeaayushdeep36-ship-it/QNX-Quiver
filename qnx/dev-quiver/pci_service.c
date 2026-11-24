#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>

#include <pci/pci.h>

#include "quiver_protocol.h"

#define QUIVER_PCI_VENDOR 0x1af4u
#define QUIVER_PCI_DEVICE 0x1110u
#define QUIVER_BAR2_NUMBER 2

static int pci_failure(const char *operation, pci_err_t error)
{
    fprintf(stderr, "QNX-Quiver: %s failed: %s (%d)\n",
            operation, pci_strerror(error), error);
    return EXIT_FAILURE;
}

int main(void)
{
    pci_err_t error = PCI_ERR_OK;
    pci_bdf_t bdf;
    pci_devhdl_t handle = NULL;
    pci_ba_t bars[6] = {{0}};
    int bar_count = (int)(sizeof(bars) / sizeof(bars[0]));
    pci_ba_t *bar2 = NULL;
    void *mapping = MAP_FAILED;
    int result = EXIT_FAILURE;

    if (ThreadCtl(_NTO_TCTL_IO, 0) == -1) {
        fprintf(stderr, "QNX-Quiver: ThreadCtl(_NTO_TCTL_IO) failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }

    bdf = pci_device_find(0, QUIVER_PCI_VENDOR, QUIVER_PCI_DEVICE, PCI_CCODE_ANY);
    if (bdf == PCI_BDF_NONE) {
        fprintf(stderr, "QNX-Quiver: %04x:%04x was not found\n",
                QUIVER_PCI_VENDOR, QUIVER_PCI_DEVICE);
        return EXIT_FAILURE;
    }
    printf("QNX-Quiver: found %04x:%04x; attaching as exclusive owner\n",
           QUIVER_PCI_VENDOR, QUIVER_PCI_DEVICE);

    handle = pci_device_attach(bdf, pci_attachFlags_EXCLUSIVE_OWNER, &error);
    if (handle == NULL) {
        return pci_failure("pci_device_attach", error);
    }

    /* pci_device_read_ba requires ownership and a real array with capacity. */
    error = pci_device_read_ba(handle, &bar_count, bars,
                              pci_reqType_e_UNSPECIFIED);
    if (error != PCI_ERR_OK) {
        result = pci_failure("pci_device_read_ba", error);
        goto done;
    }
    if (bar_count < 0 || bar_count > (int)(sizeof(bars) / sizeof(bars[0]))) {
        fprintf(stderr, "QNX-Quiver: invalid BAR count returned: %d\n", bar_count);
        goto done;
    }

    for (int index = 0; index < bar_count; ++index) {
        printf("QNX-Quiver: BAR%d type=%d addr=0x%" PRIx64 " size=0x%" PRIx64 "\n",
               bars[index].bar_num, bars[index].type,
               (uint64_t)bars[index].addr, bars[index].size);
        if (bars[index].bar_num == QUIVER_BAR2_NUMBER) {
            bar2 = &bars[index];
        }
    }
    if (bar2 == NULL || bar2->type != pci_asType_e_MEM ||
        bar2->size < QV_BAR2_BYTES) {
        fprintf(stderr, "QNX-Quiver: BAR2 is missing, not memory, or smaller than %u bytes\n",
                QV_BAR2_BYTES);
        goto done;
    }

    mapping = mmap_device_memory(NULL, QV_BAR2_BYTES,
                                 PROT_READ | PROT_NOCACHE, 0,
                                 (uint64_t)bar2->addr);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "QNX-Quiver: mmap_device_memory(BAR2) failed: %s\n",
                strerror(errno));
        goto done;
    }

    /* Copy fields out instead of treating device memory as an atomic C object. */
    uint32_t magic;
    uint16_t version;
    uint16_t superblock_bytes;
    uint32_t shared_bytes;
    memcpy(&magic, (const uint8_t *)mapping + 0, sizeof(magic));
    memcpy(&version, (const uint8_t *)mapping + 4, sizeof(version));
    memcpy(&superblock_bytes, (const uint8_t *)mapping + 6,
           sizeof(superblock_bytes));
    memcpy(&shared_bytes, (const uint8_t *)mapping + 8, sizeof(shared_bytes));

    if (magic != QV_PROTOCOL_MAGIC || version != QV_PROTOCOL_VERSION ||
        superblock_bytes != sizeof(qv_superblock_t) ||
        shared_bytes != QV_BAR2_BYTES) {
        fprintf(stderr,
                "QNX-Quiver: protocol mismatch: magic=0x%08" PRIx32
                " version=%u superblock=%u shared=%u\n",
                magic, version, superblock_bytes, shared_bytes);
        goto done;
    }

    printf("QNX-Quiver: QVB1 protocol v%u verified, BAR2=%u bytes\n",
           version, shared_bytes);
    result = EXIT_SUCCESS;

done:
    if (mapping != MAP_FAILED && munmap(mapping, QV_BAR2_BYTES) == -1) {
        fprintf(stderr, "QNX-Quiver: munmap(BAR2) failed: %s\n", strerror(errno));
        result = EXIT_FAILURE;
    }
    if (handle != NULL) {
        error = pci_device_detach(handle);
        if (error != PCI_ERR_OK) {
            (void)pci_failure("pci_device_detach", error);
            result = EXIT_FAILURE;
        }
    }
    return result;
}
