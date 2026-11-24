# Tests

## Host protocol suite

Run `python -m unittest discover -s tests -v` or `make host-test`. The tests cover protocol v3 initialization and rejection of old/malformed mappings, ring full/wraparound and serialized work-sharing, fault snapshot timeout after a stalled writer, CAN-FD record validation, actuator bounds/acknowledgement/rejection/TTL-to-zero, and concurrent host counter updates. `python host/sim_engine/quiver_sim.py --self-test` also checks ring capacity, wraparound, fault controls, and concurrent updates.

## QNX guest checks

`qnx/dev-quiver/client_probe.c` exercises the PCI client API. `qnx/dev-can-quiver/can_probe.c` exercises CAN-FD loopback, blocking RX wake, node draining, and CAN state queries. The v3 binaries compile in the installed SDP 8.0 toolchain, but the new v3 protocol/services still need deployment to the guest for end-to-end runtime checks.

## QEMU PCI lifecycle

The QMP harness checks `DEVICE_DELETED`, PCI inventory, BAR geometry, and BAR addresses. The existing PCI-bridge path can be exercised with `python3 tests/qemu_pci_hotplug.py --port 4443`; guest-side acceptance additionally requires the watcher to observe `/dev/quiver` withdrawal and republication.

On the current QNX 8.0 + QEMU 10.2.1 setup, the default i440fx configuration does not complete removal. A full test IFS with event-handler and hotplug bus-configuration modules does not fix the PCI-bridge path. On the i440fx root bus, a host-injected ACPI eject acknowledgment (`HMP o /w 0xae08 0x10`) makes QEMU remove the function and the QNX manager withdraw `/dev/quiver`; this does not prove QNX handled the eject event. Reinsertion enumerates in QEMU but both BAR addresses remain `-1`, so the QNX manager cannot remap BAR2 and the harness reports `UNVERIFIED`. For this root-bus experiment use `python3 tests/qemu_pci_hotplug.py --port 4444 --bus pci.0 --address 0x4 --memdev qnx_shm_reinsert --acpi-eject-ack`. Each reinsert attempt needs a fresh QEMU memory-backend object ID mapped to the same shared-memory file; QEMU 10.2.1 asserts if an attachment reuses the same backend object. PCI-server stop/restart remains separate passing recovery evidence. No physical hardware is required to continue this software-only investigation.
