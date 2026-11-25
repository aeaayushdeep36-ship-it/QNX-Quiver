# Tests

## Host protocol suite

Run `python -m unittest discover -s tests -v` or `make host-test`. The tests cover protocol v3 initialization and rejection of old or malformed mappings, ring full/wraparound and serialized work-sharing, fault snapshot timeout after a stalled writer, CAN-FD record validation, actuator bounds/acknowledgement/rejection/TTL-to-zero, concurrent host counter updates, and PCI-slot lookup for ACPI eject handling. `python host/sim_engine/quiver_sim.py --self-test` checks ring capacity, wraparound, fault controls, and concurrent updates.

## QNX guest checks

`qnx/dev-quiver/client_probe.c` exercises the PCI client API. `qnx/dev-can-quiver/can_probe.c` exercises CAN-FD loopback, blocking RX wake, node draining, and CAN state queries. The v3 resource manager, client probe, CAN FD echo and state transitions, IMU delivery, actuator acknowledgement/rejection/expiry, trace capture, controller stale-data fail-safe, and supervisor readiness/child-restart/shutdown have passed disposable QNX guest checks. A deliberate PCI-manager-process outage, PCI-server restart/recovery on the Q35 bridge setup, and two repeated supervisor cold starts also passed; dependent clients were stopped and relaunched after `/dev/quiver` returned. The manager detects a changed `/dev/pci/server_id_1` identity and re-executes to initialize fresh libpci state. The CAN saturation probe filled the 4,094-frame TX ring, observed `EAGAIN`, then confirmed simulator restart drained it.

## QEMU PCI lifecycle

The QMP harness checks `DEVICE_DELETED`, PCI inventory, BAR geometry, and BAR addresses. It creates a fresh memory-backend object mapped to the supplied shared file before removal, derives the ACPI eject bitmap bit from live PCI inventory, and reuses the original root-bus slot on reinsertion. Run it inside WSL with the QEMU-visible backing-file path:

```sh
python3 tests/qemu_pci_hotplug.py --port 4443 --bus pci.0 \
  --memdev qnx_shm_reinsert \
  --mem-path '/mnt/d/Freelance/QNX Quiver/build/qnx_quiver_shm_v3.bin' \
  --acpi-eject-ack
```

A host-injected ACPI acknowledgment is diagnostic only and does not prove QNX generated or processed the eject acknowledgment. Guest-side acceptance also requires observing `/dev/quiver` withdrawal and republication.

On QNX 8.0 + QEMU 10.2.1, native removal still does not complete on the tested i440fx and Q35 bridge configurations. A controlled PCI-only test found that the PCI event handler reports link events unsupported; live insertion remains absent from QNX's PCI database and BARs stay unassigned until PCI-server restart. Restarting the PCI server with the device present re-enumerates it and assigns both BARs. The resident v3 manager detects the server-instance identity change, withdraws/recreates its libpci state, republishes `/dev/quiver`, and passes the client probe. Host-acknowledged removal can withdraw the node but does not prove QNX handled the eject. The harness derives the eject bit from live PCI inventory and creates a fresh backend mapped to the same BAR2 file. Native guest-visible hot-unplug/reinsert remains unverified; no physical hardware is required for this software-only investigation. A prior i440fx PCI-server restart did trigger a QEMU assertion, while the controlled Q35 bridge restart passed.

Additional Q35 checks showed that an online test manager can own the ivshmem device and publish `/dev/quiver`, while QMP removal does not complete and QMP hot-add leaves BARs unassigned and the QNX PCI database unchanged. The bundled PCI probe and `pci-tool` are available for guest enumeration and ownership diagnostics.
