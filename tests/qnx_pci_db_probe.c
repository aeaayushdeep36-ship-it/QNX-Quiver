#include <sys/types.h>
#include <pci/pci.h>

#include <stdio.h>

int main(void)
{
    uint_t index;
    uint_t found = 0;
    pci_bdf_t bdf;
    pci_err_t error = PCI_ERR_OK;
    pci_devhdl_t handle;
    pci_ba_t bars[6] = {{0}};
    int bar_count = 6;
    int bar_index;

    for (index = 0; index < 256; ++index) {
        bdf = pci_device_find(index, PCI_VID_ANY, PCI_DID_ANY,
                              PCI_CCODE_ANY);
        if (bdf == PCI_BDF_NONE) {
            break;
        }
        printf("PCI_DB[%u]=B%u:D%u:F%u\n", index, PCI_BUS(bdf),
               PCI_DEV(bdf), PCI_FUNC(bdf));
        ++found;
    }

    bdf = pci_device_find(0, 0x1af4, 0x1110, PCI_CCODE_ANY);
    if (bdf == PCI_BDF_NONE) {
        puts("QUIVER_DEVICE=absent");
    } else {
        printf("QUIVER_DEVICE=B%u:D%u:F%u\n", PCI_BUS(bdf),
               PCI_DEV(bdf), PCI_FUNC(bdf));
        handle = pci_device_attach(bdf, pci_attachFlags_EXCLUSIVE_OWNER,
                                   &error);
        if (handle == NULL) {
            printf("QUIVER_ATTACH=failed:%s (%d)\n", pci_strerror(error),
                   error);
        } else {
            puts("QUIVER_ATTACH=ok");
            error = pci_device_read_ba(handle, &bar_count, bars,
                                       pci_reqType_e_UNSPECIFIED);
            if (error != PCI_ERR_OK) {
                printf("QUIVER_READ_BA=failed:%s (%d)\n",
                       pci_strerror(error), error);
            } else {
                printf("QUIVER_BAR_COUNT=%d\n", bar_count);
                for (bar_index = 0; bar_index < bar_count; ++bar_index) {
                    printf("QUIVER_BAR[%d]=type:%d address:0x%" PRIx64
                           " size:0x%" PRIx64 "\n", bars[bar_index].bar_num,
                           bars[bar_index].type,
                           (uint64_t)bars[bar_index].addr,
                           (uint64_t)bars[bar_index].size);
                }
            }
            (void)pci_device_detach(handle);
        }
    }

    printf("PCI_DB_COUNT=%u\n", found);
    return found == 0 ? 1 : 0;
}
