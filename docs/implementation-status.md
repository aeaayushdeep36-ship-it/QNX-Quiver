# QNX-Quiver implementation status

Updated 24 November 2026. Source completion, compile checks, host tests, and guest runtime evidence are listed separately; a successful build is not treated as guest verification.

## Implemented in source

- BAR2 protocol v3 and client API v3 preserve the ring offsets while adding snapshot generations and CAN, IMU, and actuator telemetry. Multi-client readers have explicit serialized work-sharing semantics.
- The QNX PCI resource manager owns BAR2, keeps it private, validates the protocol, withdraws `/dev/quiver` on PCI-server/device loss, and reacquires after recovery. Read-only calls may reopen and revalidate a recovered service; state-changing ring requests are never replayed after ambiguous disconnects.
- CAN FD DDK code maps extended identifiers and legal lengths through the rings. TX retries ring-full errors for a bounded 5 ms, RX queue overflow discards the oldest frame, both losses are counted, and bus-off/recovery state is exposed with a transition generation.
- Host snapshots use a bounded retry. The host simulator validates CAN format, rejects unsafe actuator commands, acknowledges accepted/rejected command IDs, expires outputs to zero, counts IMU ring drops, and reports measured rate, wake-lateness percentiles, and missed deadlines. QNX CAN, IMU, and actuator drop counters are exported through the same snapshot API.
- QNX utilities consume timestamped IMU data and detect non-advancing source timestamps, publish bounded actuator commands and wait for acknowledgement, capture fault/counter traces, and provide a controller demonstration that emits zero after a stale-sample timeout.
- A development supervisor starts the PCI manager before the CAN, IMU, and trace processes, restarts failed children with capped backoff, and coordinates shutdown. Children inherit its process type, so this supervisor is not a production least-privilege launcher.
- The WSL launcher can place ivshmem behind a PCI bridge. A QMP script requires `DEVICE_DELETED`, checks device removal/reinsert in `query-pci`, and a QNX helper waits for `/dev/quiver` withdrawal/reacquisition.
- The security policy fragment now declares process types for the added services and compiles as a fragment. It is not the guest's active policy.

## Verified on the development host

- QNX SDP 8.0 x86_64 `make clean all` completes with `-Wall -Wextra -Werror`, including all service and supervisor binaries and the policy fragment.
- Nine Python unit tests pass. They cover v3 initialization, invalid mapping/version rejection, serialized multi-reader work sharing, QNX drop metrics, stalled snapshot timeout, CAN-FD validation and bus-off hold/recovery, IMU-stuck timestamps, actuator safety/acknowledgement/expiry, and simulator operation. The simulator's standalone protocol self-test also passes.
- Two-second Windows timing runs at a 1 kHz target measured about 998.5–999.0 Hz without injected delay (p95 wake lateness about 612–648 microseconds, 2–3 misses per second). With a repeated 500 microsecond delay, the run measured about 877–878 Hz and about 856–862 missed deadlines per second. Treat these as host characterization only; no hard real-time threshold is claimed.
- The bridged WSL QEMU 10.2.1 guest boots and `query-pci` shows ivshmem behind `quiver_bridge` on bus 1 with BAR0 256 bytes and BAR2 4 MiB.
- The default-configuration guest returned successfully from QMP `device_del` but produced no `DEVICE_DELETED` event; `query-pci` still listed `quiver_ivshmem`.
- A throwaway test IFS was built with matching QNX 8.0 PCI server/libpci/hardware/capability modules plus `pci_server-event_handler.so` and `pci_server-buscfg-hotplug.so`. The PCI server started, but logged that link events were unsupported. A second QMP `device_del` still produced no `DEVICE_DELETED` event and left `quiver_ivshmem` in `query-pci`; the guest kept `/dev/quiver`. The current i440fx PCI-bridge hotplug path is therefore not verified, even with the configured modules.
- Software-only hotplug needs no physical device, but does need a QEMU bus topology that QNX's PCI server can reconfigure and report events for. The full acceptance gate remains: QMP removal event, device absent from inventory, `/dev/quiver` withdrawal without a fault, device reinserted, QMP inventory restored, and `/dev/quiver` republished. PCI-server stop/restart recovery passed separately.

## Runtime gates and limits

- The latest protocol v3 and new IMU/actuator/trace/controller/supervisor programs have been cross-compiled and host-tested, but have not yet been deployed into the supplied QNX guest for end-to-end v3 verification.
- The supplied guest has no active `/proc/boot/secpol.bin`. The exact PCI-server type/connect permission depends on the product image. Merge the fragment, identify the PCI-server type with `secpolmonitor`, assign per-service types, and test both permitted and denied operations before production.
- CAN-FD loopback and blocked RX scheduling passed on the earlier guest build. This proves the virtual path only, not physical arbitration, transceiver timing, or electrical behavior.
- Timing output reports p50/p95/p99/max wake lateness, measured rate, and deadline misses. Results from the Python simulator on a general-purpose Windows/WSL host are characterization data, not a real-time acceptance guarantee.
- The verified target remains QNX SDP 8.0 x86_64. Other architectures and cache-coherency models are unsupported until separately specified and tested.
