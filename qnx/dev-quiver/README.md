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

PCI-server stop/restart recovery passed in the earlier QNX guest build: `/dev/quiver` disappeared after `PCI_ERR_SERVER_NOT_AVAILABLE`, then returned after `pci-server` restarted. That runtime test predates protocol v3. The v3 sources cross-compile, but need a fresh deployment/runtime pass.

The current bridged QEMU test used the guest's default PCI-server config. QMP accepted `device_del`, but did not emit `DEVICE_DELETED` and `query-pci` still listed ivshmem. Restart the PCI server using `../pci_server_hotplug.cfg` and use `../pci_hotplug_wait.sh` before treating virtual remove/reinsert as verified. No physical link-loss test has been performed.

## One-shot bring-up and fault investigation

`pci_service.c` remains a one-shot attach/map/protocol smoke check (`make pci-service`). The earlier memory fault was reproduced when `ThreadCtl(_NTO_TCTL_IO, 0)` was omitted; the same PCI path succeeds after acquiring I/O privilege. An earlier probe also used `pci_device_cfg_rd32()` for BAR0 offset `0x10`, outside its documented device-specific config range. `pci_device_read_ba()` is used to retrieve BARs. Details are in `docs/pci-service-investigation.md`.
