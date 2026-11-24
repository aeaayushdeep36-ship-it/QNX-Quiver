# QNX target components

The QNX SDP 8.0 compiler and CAN DDK are present on the development host. Target driver implementation depends on a guest-visible ivshmem PCI device. The installed QEMU 11.1.0 build does not provide that device, and the QNX guest has not yet been booted with the required BAR2 bridge.

This project deliberately does not provide a host mock or claim unverified QNX device nodes. Implement the PCI resource manager and CAN DDK frontend after an ivshmem-capable QEMU image is available and the guest reports the expected PCI function and BARs.