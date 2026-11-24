# QNX-Quiver PCI service and client API

## Resident resource manager

`pci_rm.c` exclusively owns the QEMU ivshmem PCI function, maps BAR2 as noncached device memory, validates QVB1 v3, and publishes `/dev/quiver`. BAR2 remains private to the manager. Its `devctl` API validates ring direction and request shape. SIGINT/SIGTERM/SIGHUP withdraw the node and release resources. Once resident, it checks PCI health once per second, withdraws the node on device or PCI-server loss, and republishes only after a fresh attach/map/protocol validation.

## Client API

Build with `make pci-client client-probe`. Client calls return zero on success or a positive QNX error number.

- `qv_client_get_info()` reports API/protocol versions, BAR size, generation, and ring geometry.
- `qv_client_ring_pop()` reads CAN RX or IMU RX. Multiple processes work-share the single serialized ring consumer; each record goes to one client only.
- `qv_client_ring_push()` publishes CAN TX or actuator TX. An ambiguous disconnect is returned to the caller and never replayed.
- `qv_client_get_faults()` provides a bounded, sequence-consistent snapshot of fault flags, latency, CAN/IMU/actuator counters, command IDs, and actuator output.
- `qv_client_counter_add()` updates only QNX-owned CAN TX/RX/drop and recovery counters.

Info and fault reads may reopen a disconnected descriptor, verify the API/QVB1 version, and retry the read once. Ring mutations are not retried because the manager may have completed a request just before disconnect.

## Build and start

In QNX SDP 8.0 run `make pci-rm`, transfer `build/quiver-pci-rm` into the guest, and start it after `pci-server` is available:

```sh
/tmp/quiver-pci-rm
cat /dev/quiver
```

It remains in the foreground. Stop it with `slay -s SIGTERM quiver-pci-rm`; the node disappears. Starting it again reacquires the device if the PCI server and function are present.

## Recovery evidence

PCI-server stop/restart recovery passed in an earlier QNX guest build. In the current v3 disposable guest, stopping `pci-server` withdrew `/dev/quiver` after `PCI_ERR_SERVER_NOT_AVAILABLE`; restarting it triggered a QEMU 10.2.1 i440fx `iotlb_to_section` assertion before node republication could be checked. Full v3 server-restart recovery remains open on this QEMU topology.

The default guest and hotplug-module test IFS do not complete removal on the current i440fx PCI-bridge topology. On i440fx root bus, QEMU removes the function only after a host-injected ACPI eject acknowledgment; the manager then withdraws `/dev/quiver`, but this does not prove QNX handled the eject event. Reinsertion appears in QEMU but BAR0/BAR2 are unassigned, so the manager correctly stays offline. The updated QMP test reports that as `UNVERIFIED`. PCI-server stop/restart succeeded on an earlier guest build; the current v3 retry hit a QEMU assertion before recovery could be checked. Full PCI hot-unplug/reinsert remains open and can be investigated without physical hardware.

## One-shot bring-up and fault investigation

`pci_service.c` remains a one-shot attach/map/protocol smoke check (`make pci-service`). The earlier memory fault was reproduced when `ThreadCtl(_NTO_TCTL_IO, 0)` was omitted; the same PCI path succeeds after acquiring I/O privilege. An earlier probe also used `pci_device_cfg_rd32()` for BAR0 offset `0x10`, outside its documented device-specific config range. `pci_device_read_ba()` is used to retrieve BARs. Details are in `docs/pci-service-investigation.md`.
