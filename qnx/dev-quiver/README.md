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

In a Q35 disposable guest with a PCIe-to-PCI bridge, a controlled PCI-only test showed that live insertion is not delivered to QNX's PCI database on this topology. Restarting `pci-server` while ivshmem was present caused QNX to enumerate the function and assign BAR0 and BAR2. The resident manager detects a change in the PCI server's `/dev/pci/server_id_1` identity, withdraws `/dev/quiver`, cleans up its timer and dispatch state, then re-executes itself so libpci is initialized against the new server instance. The same manager PID then republishes `/dev/quiver`, and the v3 client probe passes. Its timer pulse runs the health and server-identity checks independently of client requests. Set `QNX_QUIVER_PCI_SERVER_NODE` if the target image uses a different PCI-server resource path; the default is `/dev/pci/server_id_1`.

Native guest-visible hot-unplug and a full remove/reinsert cycle remain unverified. On the tested QNX SDP 8.0 + QEMU 10.2.1 setup, the PCI event handler reports that link events are unsupported for the tested bridge, and live insertion does not update QNX's PCI database. Host-acknowledged QEMU removal can remove the function but does not prove QNX generated the acknowledgment. Reinsertion may enumerate in QEMU without BAR assignment until `pci-server` is restarted. The software recovery path after PCI-server restart is verified; a supported QEMU topology or explicit software limitation is still needed for native hot-unplug acceptance.

## One-shot bring-up and fault investigation

`pci_service.c` remains a one-shot attach/map/protocol smoke check (`make pci-service`). The earlier memory fault was reproduced when `ThreadCtl(_NTO_TCTL_IO, 0)` was omitted; the same PCI path succeeds after acquiring I/O privilege. An earlier probe also used `pci_device_cfg_rd32()` for BAR0 offset `0x10`, outside its documented device-specific config range. `pci_device_read_ba()` is used to retrieve BARs. Details are in `docs/pci-service-investigation.md`.
