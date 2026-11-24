# QNX-Quiver PCI services

## Resident resource manager

`pci_rm.c` owns the QEMU ivshmem PCI function for its lifetime, maps BAR2 as noncached device memory, validates the QVB1 header, and registers `/dev/quiver`. BAR2 remains private to the resource manager; clients use status reads and the versioned `devctl` API in `include/quiver_client.h`. The node is mode `0660`. SIGINT, SIGTERM, and SIGHUP detach the name, unmap BAR2, and detach from the PCI server.

## Client API

Build the QNX client object and read-only probe with:

    make pci-client client-probe

The public API is declared in `include/quiver_client.h` and implemented in `client.c`. API calls return `0` on success or a positive QNX error number. Open read-only for info, fault snapshots, and ring consumption; open read/write for ring production and QNX telemetry updates.

- `qv_client_get_info()` reports API/QVB1 versions, BAR size, generation, and ring geometry.
- `qv_client_ring_pop()` consumes one slot from host-to-QNX CAN RX or IMU RX. It returns `EAGAIN` when empty and `EPROTO` for invalid ring metadata or a mismatched slot type.
- `qv_client_ring_push()` publishes one CAN TX or actuator TX slot. It returns `EAGAIN` on a full ring and rejects writes to host-owned rings.
- `qv_client_get_faults()` reads a snapshot of the fault flags, latency setting, and counters.
- `qv_client_counter_add()` updates only QNX-owned CAN TX/RX and recovery counters. Fault flags and latency remain host-owned and cannot be changed through this client API.

`build/quiver-client-probe` exercises info and fault snapshot reads without consuming or publishing ring data.

While resident, it checks the BAR assignment once per second. If a PCI API call reports that the PCI server has stopped, or BAR2 is no longer valid, it withdraws `/dev/quiver` and releases or invalidates its old mapping/handle. It keeps running and retries discovery. Once the device is available through a running PCI server again, it validates BAR2 and QVB1 before publishing `/dev/quiver` again. Start the service when the PCI server and device are present; startup without them fails with a diagnostic.

Build with QNX SDP 8.0 from the project root:

    make pci-rm

Copy `build/quiver-pci-rm` to the guest and start it with the required QNX I/O and physical-memory privileges:

    /tmp/quiver-pci-rm

It remains in the foreground. In a second shell, inspect it with:

    cat /dev/quiver

Stop it with SIGTERM (for example `slay -s SIGTERM quiver-pci-rm`). The node disappears when the service exits. Starting it again should reacquire the PCI device and recreate the node.

## Recovery behavior verified in the QNX guest

With the manager attached, terminating and restarting `pci-server` was verified in the WSL QEMU guest. The manager stayed alive; `/dev/quiver` disappeared after `PCI_ERR_SERVER_NOT_AVAILABLE`, then returned after `pci-server` restarted. A read after recovery reported device `1af4:1110`, BAR2 size 4 MiB, and QVB1 v1. SIGTERM after recovery removed the node and exited cleanly.

QEMU 10.2.1 accepted `device_del quiver_ivshmem` on this i440FX machine but continued to list the function in `query-pci`, so physical PCI hot-unplug/reinsert could not be exercised in this guest configuration. The test verifies PCI-server disconnect/reconnect handling. BAR2 loss caused by actual hardware removal and hot-remove notifications remain unverified. Startup with the PCI server/device absent fails; restart the manager after they are available.

## One-shot bring-up check

`pci_service.c` is retained as a one-shot attach/map/protocol smoke check. Build it with `make pci-service`; it validates and exits without registering a device node.

## Fault investigation

The earlier memory fault was reproduced when `ThreadCtl(_NTO_TCTL_IO, 0)` was omitted; the same PCI path succeeds when I/O privilege is acquired first. The earlier probe also tried to read BAR0 config offset `0x10` with `pci_device_cfg_rd32()`, which is outside that API's documented device-specific config range (starting at `0x40`). Use the owner-only `pci_device_read_ba()` API to retrieve BARs. See `docs/pci-service-investigation.md` for details.
