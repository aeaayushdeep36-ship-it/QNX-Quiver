# QNX-Quiver CAN DDK frontend

`quiver_can.c` adapts QVB1 v3 rings to QNX SDP 8.0 `libcan` and publishes `/dev/can0/rx0` plus `/dev/can0/tx0`. The data plane uses `/dev/quiver`; the frontend never maps BAR2. CAN payloads preserve extended identifiers and up to 64 data bytes. Both the QNX and host paths validate legal CAN-FD DLCs, ID range/format, and remote-frame policy.

The RX worker drains the shared ring into a bounded 64-frame queue. Blocking RX waits on a condition variable; nonblocking RX returns `EAGAIN`. If full, the oldest queued frame is discarded and the QNX drop counter increments. TX retries a full ring for up to 5 ms, then counts/logs a drop. Ambiguous disconnects are not replayed. Bus-off state and a transition generation are available through `QV_CAN_DCMD_GET_STATUS`; `build/quiver-can-probe --status` queries it.

The supplied QNX image rejects a second libcan node attach in one process, so start the endpoints separately:

```sh
build/dev-can-quiver rx &
build/dev-can-quiver tx &
build/quiver-can-probe
```

The probe sends an extended 64-byte frame and verifies byte-for-byte loopback and counters. This is a virtual-bus check; it does not test physical arbitration, bit timing, error counters, or transceiver behavior.

The policy fragment includes a CAN process type but is not active in the supplied guest. Merge it with the target policy, add the image-specific PCI-server channel permission, and verify allowed/denied access under enforcement before production.
