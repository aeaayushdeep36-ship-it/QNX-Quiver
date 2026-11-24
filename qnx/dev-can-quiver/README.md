# QNX-Quiver CAN DDK frontend

`quiver_can.c` adapts the QVB1 CAN rings to QNX SDP 8.0 `libcan`. It publishes
`/dev/can0/rx0` and `/dev/can0/tx0` as standard CAN raw-frame endpoints. The
CAN device is virtual: the only data plane is the private `/dev/quiver` client
API, so the CAN frontend never maps or accesses PCI BAR2 directly.

The host simulator's CAN record is 72 bytes: a little-endian 32-bit message ID,
data length, flags, a reserved 16-bit field, then 64 data bytes. The QNX adapter
enables `CANDEV_CFLAG_CANFD`, preserves extended identifiers and the CRC marker,
and validates CAN-FD DLC lengths (0–8, 12, 16, 20, 24, 32, 48, or 64). Remote
frames are rejected because CAN FD does not support them. Host RX arrives on
QVB1 ring 0. QNX TX is written to ring 1; the host simulator validates and echoes
it to ring 0 so all 64 data bytes can be verified on the guest.

An RX worker drains the shared ring into a bounded 64-frame queue. The blocking
raw RX devctl waits on that queue; nonblocking RX returns `EAGAIN` when empty.
This keeps shared-memory polling out of libcan's request callback. On queue
overflow, the oldest pending RX frame is dropped and counted locally.
On SIGTERM the frontend stops the poller, wakes a blocked reader with
`ECANCELED`, and allows libcan's resource-manager loop to shut down so its nodes
are removed cleanly.

Build with `make can-ddk can-probe`. Start the PCI resource manager first, then
run one `build/dev-can-quiver tx` and one `build/dev-can-quiver rx` process in
the QNX guest. QNX SDP 8.0's `libcan` publishes only one endpoint successfully
per process in the current target image; trying to publish both from one process
returns `EPERM` on the second attach. Separate endpoint processes are therefore
used, while both still use the official libcan resource manager API. With
`quiver_sim.py` running on the same shared-memory backing file, run
`build/quiver-can-probe` in the guest. The probe transmits an extended ID with
64 unique data bytes, waits for simulator loopback, verifies the complete
frame, and checks that both QNX TX and host receive counters advanced. It uses
the blocking RX devctl to exercise the worker/queue path.

`qnx/security/qnx_quiver.policy` defines least-privilege process types,
resource-manager attach paths, channel permissions, and root-only node
permissions. It is a policy fragment, not a bootable system policy: merge it
with the target image baseline and add the product-specific PCI-server channel
permission before compiling/pushing the final policy. The current development
image has no active security policy, so policy syntax is checked but cannot be
runtime-enforced in the existing guest.

The probe requires active host simulator traffic and a running `/dev/quiver`
manager. The current QEMU i440FX topology still has not verified physical PCI
hot-unplug/reinsert.
