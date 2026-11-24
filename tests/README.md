# Tests

## Host protocol suite

Run `python -m unittest discover -s tests -v` or `make host-test`. The tests cover protocol v3 initialization and rejection of old/malformed mappings, ring full/wraparound and serialized work-sharing, fault snapshot timeout after a stalled writer, CAN-FD record validation, actuator bounds/acknowledgement/rejection/TTL-to-zero, and concurrent host counter updates. `python host/sim_engine/quiver_sim.py --self-test` also checks ring capacity, wraparound, fault controls, and concurrent updates.

## QNX guest checks

`qnx/dev-quiver/client_probe.c` exercises the PCI client API. `qnx/dev-can-quiver/can_probe.c` exercises CAN-FD loopback, blocking RX wake, node draining, and CAN state queries. The v3 binaries compile in the installed SDP 8.0 toolchain, but the new v3 protocol/services still need deployment to the guest for end-to-end runtime checks.

## QEMU PCI lifecycle

Boot the WSL guest with `QNX_PCI_TOPOLOGY=pci-bridge-hotplug` and a test IFS loading the modules in `qnx/pci_server_hotplug.cfg`. Start the guest watcher with `sh /tmp/pci_hotplug_wait.sh removed 30`, then run `python3 tests/qemu_pci_hotplug.py --port 4443`. The QMP test requires a `DEVICE_DELETED` event, holds the function absent for three seconds by default, and validates removal/reinsert in `query-pci`. Run the watcher again for `reinserted 30` after QMP finishes. A passed QMP-only test does not establish that the PCI manager safely released and reacquired BAR2; all guest-side checks are required.

Current result: the supplied default config and the configured hotplug test IFS both failed to complete removal on the i440fx PCI bridge. Even with the matching QNX event-handler and bus-configuration modules, QMP accepted `device_del` without a `DEVICE_DELETED` event, and `query-pci` still listed ivshmem. The guest retained `/dev/quiver`. PCI-server stop/restart recovery is a separate passing test; do not report it as PCI hot-unplug coverage.
