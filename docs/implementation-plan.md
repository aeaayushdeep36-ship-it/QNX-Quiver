# QNX-Quiver implementation plan

## Objective

Build a QNX SDP 8.0 x86_64 virtual embedded platform that connects a deterministic host plant to QNX services through an ivshmem BAR2 protocol. QNX clients use `/dev/quiver`; BAR2 remains private to the PCI resource manager.

## Current implementation

- **Environment and PCI discovery:** the supported live path is Ubuntu WSL QEMU 10.2.1 with `ivshmem-plain`; Windows QEMU 11.1 lacks it. QEMU and simulator must both run in WSL for coherent shared-file access. The existing guest runs in snapshot mode.
- **Protocol and host plant:** protocol v3 and client API v3 retain ring offsets. The host simulator supports fault injection, strict CAN-FD validation, actuator command acknowledgement and TTL expiry, per-writer snapshot sequencing, and timing characterization.
- **PCI and client access:** a resident PCI manager claims the device and BAR2, publishes `/dev/quiver`, and recovers across PCI-server loss. Read-only status operations may reconnect after a verified version handshake; ambiguous ring mutations are reported without retry.
- **CAN and peripherals:** the DDK frontend offers split RX/TX processes, bounded TX retry, oldest-drop RX overflow, exported counters, and bus-off state. IMU monitoring, actuator commands, CSV trace capture, and a stale-data-safe controller example use only the service API.
- **Lifecycle test:** a WSL QEMU option puts ivshmem behind a PCI bridge; QMP and guest-node scripts cover remove/reinsert. The default config and a throwaway test IFS with the QNX event-handler and hotplug bus-configuration modules both failed to remove ivshmem under the current i440fx topology. End-to-end hotplug remains open while a QNX-supported QEMU topology is investigated.
- **Policy and deployment:** the policy fragment compiles but is not active in the supplied guest. The product PCI-server channel type must be discovered and permitted during image integration.

## Acceptance and remaining work

1. **Protocol and host checks:** require QNX x86_64 clean build, host unit tests, protocol self-test, and bounded timing runs. Do not treat the Python scheduler as hard real time.
2. **Resident PCI service:** verify QVB1 v3 info/fault reads, ring directions, stop/restart recovery, and correct device withdrawal/republication in the guest.
3. **PCI removal/reinsert:** enable QNX hotplug modules; pass the QMP event and inventory checks plus `/dev/quiver` disappearance/reappearance. If the supplied guest cannot support that path, keep the event unverified and retain PCI-server restart coverage as separate evidence.
4. **CAN and peripheral services:** run v3 CAN-FD echo, bus-off/recovery status, overflow/backpressure counters, IMU stale behavior, actuator bound rejection/acknowledgement/TTL-to-zero, trace output, and controller stale fail-safe in the guest.
5. **Security:** merge into the product boot policy, add the exact PCI-server channel rule, launch components with distinct process types, and demonstrate both allowed and denied operations under enforcement.
6. **Supervision and handoff:** run repeated startup, child failure/restart, partial outage, and shutdown exercises; follow the codelab from a clean environment.
7. **Hardware boundary:** physical CAN or PCI behavior is explicitly out of scope without hardware. QEMU virtual tests do not validate electrical behavior.

## Verified evidence and limits

`docs/implementation-status.md` records current build, host tests, the bridged QEMU inventory, and runtime gates. Prior guest evidence for PCI-server restart recovery and virtual CAN-FD echo remains valid for the earlier guest protocol build; protocol v3 and the newly added peripheral tools still need guest deployment and runtime checks.
