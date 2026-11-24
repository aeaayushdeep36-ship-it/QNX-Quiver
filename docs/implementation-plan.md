# QNX-Quiver implementation plan

## Objective

Build a QNX SDP 8.0 x86_64 virtual embedded platform that connects a deterministic host plant to QNX services through an ivshmem BAR2 protocol. QNX clients use `/dev/quiver`; BAR2 remains private to the PCI resource manager.

## Current implementation

- **Environment and PCI discovery:** the supported live path is Ubuntu WSL QEMU 10.2.1 with `ivshmem-plain`; Windows QEMU 11.1 lacks it. QEMU and simulator must both run in WSL for coherent shared-file access. The existing guest runs in snapshot mode.
- **Protocol and host plant:** protocol v3 and client API v3 retain ring offsets. The host simulator supports fault injection, strict CAN-FD validation, actuator command acknowledgement and TTL expiry, per-writer snapshot sequencing, and timing characterization.
- **PCI and client access:** a resident PCI manager claims the device and BAR2, publishes `/dev/quiver`, and recovers across PCI-server loss. Read-only status operations may reconnect after a verified version handshake; ambiguous ring mutations are reported without retry.
- **CAN and peripherals:** the DDK frontend offers split RX/TX processes, bounded TX retry, oldest-drop RX overflow, exported counters, and bus-off state. IMU monitoring, actuator commands, CSV trace capture, and a stale-data-safe controller example use only the service API.
- **Lifecycle test:** hotplug remains an open runtime gate. The supplied QNX PCI event-handler does not complete eject on the current i440fx PCI-bridge path. A full test IFS can remove a root-bus ivshmem only after a host-injected ACPI eject acknowledgment, and the QNX manager withdraws `/dev/quiver`; reinsertion restores QEMU enumeration but leaves BARs unassigned, so the QNX manager correctly remains offline. The QMP harness now rejects this as unverified.
- **Policy and deployment:** the policy fragment compiles but is not active in the supplied guest. The product PCI-server channel type must be discovered and permitted during image integration.

## Acceptance and remaining work

1. **Protocol and host checks:** require QNX x86_64 clean build, host unit tests, protocol self-test, and bounded timing runs. Do not treat the Python scheduler as hard real time.
2. **Resident PCI service:** v3 info/fault reads and exclusive ownership now pass in the disposable guest IFS. Device withdrawal after PCI-server loss passes; complete v3 recovery after server restart must be rerun on a QEMU topology that does not hit the observed iotlb assertion.
3. **PCI removal/reinsert:** identify a QEMU/QNX topology where QNX receives slot notifications and its hotplug bus-configuration module restores BARs. Pass QMP event/inventory and QNX `/dev/quiver` withdrawal/republication checks. Host-forced eject acknowledgment and PCI-server stop/restart remain separate partial evidence, not a full hotplug pass.
4. **CAN and peripheral services:** v3 CAN-FD echo, bus-off/recovery status, RX drop accounting, timestamped IMU delivery, actuator rejection/acknowledgement/TTL-to-zero, trace output, and controller stale fail-safe now have guest runtime evidence. Add a controlled TX ring saturation/backpressure exercise and repeat after restart/reconnect.
5. **Security:** merge into the product boot policy, add the exact PCI-server channel rule, launch components with distinct process types, and demonstrate both allowed and denied operations under enforcement.
6. **Supervision and handoff:** supervisor startup/readiness, trace-child restart, SIGTERM shutdown, two fresh cold starts, and PCI-manager-process outage recovery now pass in the disposable guest. Verify recovery after PCI-server loss on a QEMU topology that avoids the current QEMU assertion; follow the updated codelab from a clean environment.
7. **Hardware boundary:** physical CAN or PCI behavior is explicitly out of scope without hardware. QEMU virtual tests do not validate electrical behavior.

## Verified evidence and limits

`docs/implementation-status.md` records current build, host tests, disposable QNX guest checks, the bridged QEMU inventory, and remaining runtime gates. The current v3 manager's complete PCI-server restart recovery is not established on the QEMU 10.2.1 i440fx run because QEMU asserted during restart. Production policy enforcement still requires the target image's active policy and PCI-server process type.
