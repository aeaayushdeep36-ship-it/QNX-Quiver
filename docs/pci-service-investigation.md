# QNX-Quiver PCI service fault investigation

This report records the original one-shot probe failure and its correction. The resident manager and client API were implemented afterward; see `qnx/dev-quiver/README.md` for their current behavior.

## Finding

The old guest probe attached to the PCI device and then faulted during PCI operations. A controlled comparison established the cause: the probe did not acquire QNX I/O privilege before invoking the PCI client/mapping path. The exact same valid attach, BAR retrieval, mapping, and protocol validation implementation faults with `Memory fault` when `ThreadCtl(_NTO_TCTL_IO, 0)` is omitted, and succeeds when that call runs first.

A second issue in the old probe was attempting to read BAR0 at configuration offset `0x10` with `pci_device_cfg_rd32()`. In QNX SDP 8.0, this API is documented for device-specific configuration registers beginning at offset `0x40`. BARs must be retrieved through `pci_device_read_ba()` after an owner attach. That call needs a non-null capacity pointer and a sufficiently sized array for an unspecified BAR request; BAR2 can then be selected by `bar_num`.

## Implemented minimal service

`qnx/dev-quiver/pci_service.c` now:

1. Acquires `_NTO_TCTL_IO` before any PCI operations.
2. Finds `1af4:1110` and attaches as exclusive owner.
3. Calls `pci_device_read_ba()` with capacity for all six BAR slots.
4. Requires BAR2 to be memory mapped and at least 4 MiB.
5. Maps BAR2 using `mmap_device_memory()` with `PROT_NOCACHE`.
6. Validates protocol magic, version, superblock size, and shared size.
7. Unmaps and detaches on every post-attach path.

The original binary compiled with QNX SDP 8.0 x86_64 and ran successfully in the live QNX guest. It reported BAR0 at `0xfeb91000` (256 bytes), BAR2 at `0xfe000000` (4 MiB), validated QVB1 v1, and exited with status 0. A comparison binary without I/O privilege faulted as expected. That source remains as a one-shot smoke utility; the resident resource manager now lives in `pci_rm.c` and exposes `/dev/quiver`.

## Validation commands

From the project root with QNX SDP 8.0 configured:

    make pci-service

In the running guest after copying the binary:

    /tmp/quiver-pci-service

Expected success ends with:

    QNX-Quiver: QVB1 protocol v3 verified, BAR2=4194304 bytes
