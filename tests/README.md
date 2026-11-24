# Tests

## Host protocol suite

Run `python -m unittest discover -s tests -v` or `make host-test`. The tests cover protocol v3 initialization and rejection of old or malformed mappings, ring full/wraparound and serialized work-sharing, fault snapshot timeout after a stalled writer, CAN-FD record validation, actuator bounds/acknowledgement/rejection/TTL-to-zero, concurrent host counter updates, and PCI-slot lookup for ACPI eject handling. `python host/sim_engine/quiver_sim.py --self-test` checks ring capacity, wraparound, fault controls, and concurrent updates.

## QNX guest checks

`qnx/dev-quiver/client_probe.c` exercises the PCI client API. `qnx/dev-can-quiver/can_probe.c` exercises CAN-FD loopback, blocking RX wake, node draining, and CAN state queries. The v3 resource manager, client probe, CAN FD echo and state transitions, IMU delivery, actuator acknowledgement/rejection/expiry, trace capture, controller stale-data fail-safe, and supervisor readiness/child-restart/shutdown have passed disposable QNX guest checks. A deliberate PCI-manager-process outage and two repeated supervisor cold starts also passed; dependent clients were stopped and relaunched after `/dev/quiver` returned. The CAN saturation probe filled the 4,094-frame TX ring, observed `EAGAIN`, then confirmed simulator restart drained it. PCI-server stop/restart recovery remains unverified on the current QEMU topology.

## QEMU PCI lifecycle

The QMP harness checks `DEVICE_DELETED`, PCI inventory, BAR geometry, and BAR addresses. It creates a fresh memory-backend object mapped to the supplied shared file before removal, derives the ACPI eject bitmap bit from live PCI inventory, and reuses the original root-bus slot on reinsertion. Run it inside WSL with the QEMU-visible backing-file path:

```sh
python3 tests/qemu_pci_hotplug.py --port 4443 --bus pci.0 \
  --memdev qnx_shm_reinsert \
  --mem-path '/mnt/d/Freelance/QNX Quiver/build/qnx_quiver_shm_v3.bin' \
  --acpi-eject-ack
```

A host-injected ACPI acknowledgment is diagnostic only and does not prove QNX generated or processed the eject acknowledgment. Guest-side acceptance also requires observing `/dev/quiver` withdrawal and republication.

On QNX 8.0 + QEMU 10.2.1, default i440fx removal did not complete, and adding event-handler and hotplug bus-configuration modules did not fix the PCI-bridge path. Earlier host-acknowledged removal withdrew `/dev/quiver`, but did not prove QNX handled the eject event. Reinsertion previously appeared in QEMU with BARs unassigned, so the manager correctly stayed offline. The harness now derives the eject bit from live PCI inventory and creates a fresh backend mapped to the same BAR2 file. PCI-server stop/restart recovery is documented for an earlier guest build; the current v3 test hit a QEMU assertion during restart. Actual insertion/removal recovery remains unverified; no physical hardware is required for this software-only investigation.

Additional Q35 checks showed that an online test manager can own the ivshmem device and publish `/dev/quiver`, while QMP removal does not complete and QMP hot-add leaves BARs unassigned and the QNX PCI database unchanged. The bundled PCI probe and `pci-tool` are available for guest enumeration and ownership diagnostics.
