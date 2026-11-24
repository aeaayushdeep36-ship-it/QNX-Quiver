# QNX-Quiver environment discovery

Discovery was performed on 2026-11-24 using the local Windows PowerShell environment.

## QNX SDP 8.0

- Environment script: C:\Users\Lenovo\qnx800\qnxsdp-env.bat
- QNX_HOST: C:\Users\Lenovo\qnx800\host\win64\x86_64
- QNX_TARGET: C:\Users\Lenovo\qnx800\target\qnx
- qcc is available; gcc_ntox86_64 is the default x86_64 QNX target.
- mkqnximage is available, but the attempted help invocation failed inside the packaged MSYS bash due to a Windows named-object permission error.
- traceprinter is available.
- QNX PCI header: usr/include/pci/pci.h.
- QNX CAN DDK header: usr/include/hw/libcan.h.
- QNX CAN devctl header: usr/include/sys/can_dcmd.h.
- The x86_64 CAN DDK library and canctl are present under target/qnx/x86_64.

## QEMU and guest image

- Windows QEMU: C:\Program Files\qemu\qemu-system-x86_64.exe, version 11.1.0. WHPX and TCG are available, but this build lacks ivshmem-plain and ivshmem-doorbell.
- Ubuntu WSL QEMU: version 10.2.1, installed with qemu-system-x86 and qemu-utils. It exposes both ivshmem-plain and ivshmem-doorbell.
- The existing QNX image is C:\Users\Lenovo\qnxvm\output\disk-qemu.vmdk with IFS at C:\Users\Lenovo\qnxvm\output\ifs.bin.
- The existing QNX 8.0 image boots in QEMU snapshot mode. Without ivshmem, guest pci-tool -v showed 1af4:1000 (virtio network) and 1af4:1005 (virtio RNG), but no 1af4:1110.
- The WSL launcher uses file-backed ivshmem-plain. The running guest exposes 1af4:1110 with BAR0 (0x100 bytes) and BAR2 (4 MiB).
- QNX `pci-tool` read the QVB1 header and a live simulator ring-head update through BAR2. SSH forwarding on port 2223 was used for guest inspection.

## Verification boundary

The QNX protocol layout check cross-compiles for QNX x86_64. The host simulator self-test and a 1.2-second run pass at approximately 999 Hz. QNX PCI enumeration and BAR reads are verified via `pci-tool`; project PCI attachment code, resource managers, and device nodes remain unimplemented. Windows and WSL did not share live updates through the same DrvFS mapping, so the WSL simulator wrapper must be used alongside WSL QEMU.
